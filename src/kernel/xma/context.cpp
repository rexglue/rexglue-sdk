/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

/*
 * Derived from https://github.com/koolkdev/libertyv/blob/master/libav_wrapper/xma2dec.c
 */

#include <algorithm>
#include <cmath>
#include <cstring>

#include "kernel/xma/context.h"
#include "kernel/xma/decoder.h"
#include "kernel/xma/packet.h"
#include <rex/platform/perf.h>
#include <rex/logging/log.h>
#include <rex/platform.h>
#include <rex/memory/stream.h>
#include <rex/platform/sync.h>

#include <simde/x86/ssse3.h>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/error.h"
}  // extern "C"

namespace rex::kernel {

const u32 XMAContext::kBitsPerPacketHeader;
const u32 XMAContext::kOutputMaxSizeBytes;

XMAContext::XMAContext()
    : work_completion_event_(rex::platform::Event::CreateAutoResetEvent(false)) {}

XMAContext::~XMAContext() {
  if (av_context_) {
    avcodec_free_context(&av_context_);
  }
  if (av_frame_) {
    av_frame_free(&av_frame_);
  }
}

int XMAContext::Setup(u32 id, kernel::Memory* memory, u32 guest_ptr) {
  id_ = id;
  memory_ = memory;
  guest_ptr_ = guest_ptr;

  av_packet_ = av_packet_alloc();
  REX_ASSERT(av_packet_ != nullptr);
  av_packet_->buf = av_buffer_alloc(128 * 1024);

  av_codec_ = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  if (!av_codec_) {
    REX_FAIL(apu, 1, "XMAContext {}: Codec not found", id);
  }

  av_context_ = avcodec_alloc_context3(av_codec_);
  if (!av_context_) {
    REX_FAIL(apu, 1, "XMAContext {}: Couldn't allocate context", id);
  }

  av_context_->channels = 0;
  av_context_->sample_rate = 0;

  av_frame_ = av_frame_alloc();
  if (!av_frame_) {
    REX_FAIL(apu, 1, "XMAContext {}: Couldn't allocate frame", id);
  }

  return 0;
}

bool XMAContext::Work() {
  if (!IsAllocated() || !IsEnabled()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(lock_);
  SetEnabled(false);

  auto context_ptr = GetMemory()->TranslateVirtual(GetGuestPtr());
  XMA_CONTEXT_DATA data(context_ptr);
  const XMA_CONTEXT_DATA initial_data = data;

  if (!data.output_buffer_valid) {
    return true;
  }

  memory::RingBuffer output_rb = PrepareOutputRingBuffer(&data);

  if (data.IsConsumeOnlyContext()) {
    if (current_frame_remaining_subframes_ == 0) {
      return true;
    }
    Consume(&output_rb, &data);
    data.output_buffer_write_offset = output_rb.GetWriteOffset() / kOutputBytesPerBlock;
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }

  const u32 subframe_decode_count_min = std::max(static_cast<u32>(1), data.subframe_decode_count);
  const i32 minimum_subframe_decode_count =
      static_cast<i32>(subframe_decode_count_min) + data.output_buffer_padding;

  if (minimum_subframe_decode_count > remaining_subframe_blocks_in_output_buffer_) {
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }

  while (remaining_subframe_blocks_in_output_buffer_ >= minimum_subframe_decode_count) {
    Decode(&data);
    Consume(&output_rb, &data);

    if (!data.IsAnyInputBufferValid() || data.error_status == 4) {
      break;
    }
  }

  data.output_buffer_write_offset = output_rb.GetWriteOffset() / kOutputBytesPerBlock;

  if (output_rb.IsEmpty()) {
    data.output_buffer_valid = 0;
  }

  StoreContextMerged(data, initial_data, context_ptr);
  return true;
}

void XMAContext::Enable() {
  std::lock_guard<std::mutex> lock(lock_);
  SetEnabled(true);
}

bool XMAContext::WaitIdle(bool poll) {
  if (poll) {
    return std::unique_lock(lock_, std::try_to_lock).owns_lock();
  }
  std::scoped_lock lock(lock_);
  return true;
}

void XMAContext::Clear() {
  std::lock_guard<std::mutex> lock(lock_);
  REXLOG_DEV(apu, "XMAContext: reset context {}", GetID());

  auto context_ptr = GetMemory()->TranslateVirtual(GetGuestPtr());
  XMA_CONTEXT_DATA data(context_ptr);
  ClearLocked(&data);
  data.Store(context_ptr);
}

void XMAContext::ClearLocked(XMA_CONTEXT_DATA* data) {
  data->input_buffer_0_valid = 0;
  data->input_buffer_1_valid = 0;
  data->output_buffer_valid = 0;

  data->input_buffer_read_offset = kBitsPerPacketHeader;
  data->output_buffer_read_offset = 0;
  data->output_buffer_write_offset = 0;

  ResetDecoderState();
}

void XMAContext::ResetDecoderState() {
  if (av_context_) {
    av_context_->sample_rate = 0;
    av_context_->channels = 0;
  }
  raw_frame_.fill(0);
  decoded_frame_.fill(0);
  carry_frame_.fill(0);
  carry_valid_ = false;
  pending_output_limit_ = 0;
  pending_start_skip_ = 0;
  current_frame_remaining_subframes_ = 0;
  loop_frame_output_limit_ = 0;
  loop_start_skip_pending_ = false;
}

void XMAContext::Disable() {
  std::lock_guard<std::mutex> lock(lock_);
  SetEnabled(false);
}

void XMAContext::Release() {
  std::lock_guard<std::mutex> lock(lock_);
  REX_ASSERT(IsAllocated());

  SetAllocated(false);
  ResetDecoderState();
  auto context_ptr = GetMemory()->TranslateVirtual(GetGuestPtr());
  std::memset(context_ptr, 0, sizeof(XMA_CONTEXT_DATA));
}

void XMAContext::SwapInputBuffer(XMA_CONTEXT_DATA* data) {
  if (data->current_buffer == 0) {
    data->input_buffer_0_valid = 0;
  } else {
    data->input_buffer_1_valid = 0;
  }
  data->current_buffer ^= 1;
  data->input_buffer_read_offset = kBitsPerPacketHeader;
}

void XMAContext::UpdateLoopStatus(XMA_CONTEXT_DATA* data) {
  if (data->loop_count == 0) {
    return;
  }

  const u32 loop_start = std::max(kBitsPerPacketHeader, data->loop_start);
  const u32 loop_end = std::max(kBitsPerPacketHeader, data->loop_end);

  if (data->input_buffer_read_offset != loop_end) {
    return;
  }

  data->input_buffer_read_offset = loop_start;
  loop_start_skip_pending_ = true;

  if (data->loop_count < 255) {
    data->loop_count--;
  }
}

int XMAContext::GetSampleRate(int id) {
  return kIdToSampleRate[std::min(id, 3)];
}

i16 XMAContext::GetPacketNumber(size_t size, size_t bit_offset) {
  if (bit_offset < kBitsPerPacketHeader) {
    REX_ASSERT_ALWAYS();
    return -1;
  }
  if (bit_offset >= (size << 3)) {
    REX_ASSERT_ALWAYS();
    return -1;
  }
  size_t byte_offset = bit_offset >> 3;
  size_t packet_number = byte_offset / kBytesPerPacket;
  return static_cast<i16>(packet_number);
}

u32 XMAContext::GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data) {
  return data->GetCurrentInputBufferPacketCount() * kBytesPerPacket;
}

u8* XMAContext::GetCurrentInputBuffer(XMA_CONTEXT_DATA* data) const {
  return GetMemory()->TranslatePhysical(data->GetCurrentInputBufferAddress());
}

u32 XMAContext::GetAmountOfBitsToRead(u32 remaining_stream_bits, u32 frame_size) {
  return std::min(remaining_stream_bits, frame_size);
}

const u8* XMAContext::GetNextPacket(XMA_CONTEXT_DATA* data, u32 next_packet_index,
                                    u32 current_input_packet_count) const {
  if (next_packet_index < current_input_packet_count) {
    return GetMemory()->TranslatePhysical(data->GetCurrentInputBufferAddress()) +
           size_t{next_packet_index} * kBytesPerPacket;
  }

  const u8 next_buffer_index = data->current_buffer ^ 1;
  if (!data->IsInputBufferValid(next_buffer_index)) {
    return nullptr;
  }

  const u32 next_buffer_address = data->GetInputBufferAddress(next_buffer_index);
  if (!next_buffer_address) {
    REX_FAIL(apu, nullptr, "XMAContext {}: Buffer marked valid but has null pointer!", GetID());
  }

  return GetMemory()->TranslatePhysical(next_buffer_address);
}

u32 XMAContext::GetNextPacketReadOffset(u8* buffer, u32 next_packet_index,
                                        u32 current_input_packet_count) {
  while (next_packet_index < current_input_packet_count) {
    u8* next_packet = buffer + size_t{next_packet_index} * kBytesPerPacket;
    const u32 packet_frame_offset = xma::GetPacketFrameOffset(next_packet);

    if (packet_frame_offset <= kMaxFrameSizeInBits) {
      return (next_packet_index * kBitsPerPacket) + packet_frame_offset;
    }
    next_packet_index++;
  }

  return kBitsPerPacketHeader;
}

memory::RingBuffer XMAContext::PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data) {
  const u32 output_capacity = data->output_buffer_block_count * kOutputBytesPerBlock;
  const u32 output_read_offset = data->output_buffer_read_offset * kOutputBytesPerBlock;
  const u32 output_write_offset = data->output_buffer_write_offset * kOutputBytesPerBlock;

  if (output_capacity > kOutputMaxSizeBytes) {
    REXLOG_WARN(apu,
                "XMAContext {}: Output buffer exceeds expected size! "
                "(size {}, max {})",
                GetID(), output_capacity, kOutputMaxSizeBytes);
  }

  u8* output_buffer = GetMemory()->TranslatePhysical(data->output_buffer_ptr);

  memory::RingBuffer output_rb(output_buffer, output_capacity);
  output_rb.SetReadOffset(output_read_offset);
  output_rb.SetWriteOffset(output_write_offset);
  remaining_subframe_blocks_in_output_buffer_ =
      static_cast<i32>(output_rb.GetWriteCount() / kOutputBytesPerBlock);

  return output_rb;
}

PacketFrame XMAContext::GetPacketInfo(u8* packet, u32 frame_offset) {
  PacketFrame packet_info = {};

  const u32 first_frame_offset = xma::GetPacketFrameOffset(packet);
  memory::BitStream stream(packet, kBitsPerPacket);
  stream.SetOffset(first_frame_offset);

  if (frame_offset < first_frame_offset) {
    packet_info.current_frame = 0;
    packet_info.current_frame_size = first_frame_offset - frame_offset;
  }

  while (true) {
    if (stream.GetBitsRemaining() < kBitsPerFrameHeader) {
      break;
    }

    const u64 frame_size = stream.Peek(kBitsPerFrameHeader);
    if (frame_size == 0 || frame_size == xma::kMaxFrameLength) {
      break;
    }

    if (stream.GetOffsetBits() == frame_offset) {
      packet_info.current_frame = packet_info.frame_count;
      packet_info.current_frame_size = static_cast<u32>(frame_size);
    }

    packet_info.frame_count++;

    if (frame_size > stream.GetBitsRemaining()) {
      break;
    }

    stream.Advance(frame_size - 1);

    if (stream.Read(1) == 0) {
      break;
    }
  }

  if (xma::IsPacketXMA2Type(packet)) {
    const u8 xma2_frame_count = xma::GetPacketFrameCount(packet);
    if (xma2_frame_count > packet_info.frame_count) {
      if (packet_info.current_frame_size == 0) {
        packet_info.current_frame = packet_info.frame_count;
      }
      packet_info.frame_count = xma2_frame_count;
    }
  }
  return packet_info;
}

void XMAContext::StoreContextMerged(const XMA_CONTEXT_DATA& data,
                                    const XMA_CONTEXT_DATA& initial_data, u8* context_ptr) {
  XMA_CONTEXT_DATA fresh(context_ptr);

  fresh.loop_count = data.loop_count;
  fresh.output_buffer_write_offset = data.output_buffer_write_offset;
  if (initial_data.input_buffer_0_valid && !data.input_buffer_0_valid) {
    fresh.input_buffer_0_valid = 0;
  }
  if (initial_data.input_buffer_1_valid && !data.input_buffer_1_valid) {
    fresh.input_buffer_1_valid = 0;
  }

  if (initial_data.output_buffer_valid && !data.output_buffer_valid) {
    fresh.output_buffer_valid = 0;
  }

  fresh.input_buffer_read_offset = data.input_buffer_read_offset;
  fresh.error_status = data.error_status;
  fresh.current_buffer = data.current_buffer;
  fresh.output_buffer_read_offset = data.output_buffer_read_offset;

  fresh.Store(context_ptr);
}

void XMAContext::Consume(memory::RingBuffer* output_rb, const XMA_CONTEXT_DATA* data) {
  if (!current_frame_remaining_subframes_) {
    return;
  }

  if (loop_frame_output_limit_ > 0) {
    const u8 total_subframes = (kBytesPerFrameChannel / kOutputBytesPerBlock) << data->is_stereo;
    const u8 consumed = total_subframes - current_frame_remaining_subframes_;
    if (consumed >= loop_frame_output_limit_) {
      remaining_subframe_blocks_in_output_buffer_ -= data->output_buffer_padding;
      current_frame_remaining_subframes_ = 0;
      loop_frame_output_limit_ = 0;
      return;
    }
  }

  const u8 subframe_decode_count_min = std::max(static_cast<u32>(1), data->subframe_decode_count);
  i8 subframes_to_write = std::min(static_cast<i8>(current_frame_remaining_subframes_),
                                   static_cast<i8>(subframe_decode_count_min));

  if (loop_frame_output_limit_ > 0) {
    const u8 total_subframes = (kBytesPerFrameChannel / kOutputBytesPerBlock) << data->is_stereo;
    const u8 consumed = total_subframes - current_frame_remaining_subframes_;
    const i8 remaining_until_limit = static_cast<i8>(loop_frame_output_limit_ - consumed);
    if (subframes_to_write > remaining_until_limit) {
      subframes_to_write = remaining_until_limit;
    }
  }

  const i8 raw_frame_read_offset = static_cast<i8>(
      static_cast<i32>((kBytesPerFrameChannel / kOutputBytesPerBlock) << data->is_stereo) -
      current_frame_remaining_subframes_);
  REX_ASSERT(raw_frame_read_offset >= 0);

  output_rb->Write(
      raw_frame_.data() + static_cast<i64>(kOutputBytesPerBlock) * raw_frame_read_offset,
      static_cast<size_t>(subframes_to_write) * kOutputBytesPerBlock);

  const i8 headroom = (current_frame_remaining_subframes_ - subframes_to_write == 0)
                          ? static_cast<i8>(data->output_buffer_padding)
                          : i8{0};

  remaining_subframe_blocks_in_output_buffer_ -= subframes_to_write + headroom;
  current_frame_remaining_subframes_ -= subframes_to_write;
}

int XMAContext::PrepareDecoder(int sample_rate, bool is_two_channel) {
  sample_rate = GetSampleRate(sample_rate);

  u32 channels = is_two_channel ? 2 : 1;
  if (av_context_->sample_rate != sample_rate ||
      av_context_->channels != static_cast<int>(channels)) {
    REXLOG_DEV(apu, "XMAContext {}: Codec reinit, rate {} to {}, channels {} to {}", GetID(),
               av_context_->sample_rate, sample_rate, av_context_->channels, channels);
    avcodec_free_context(&av_context_);
    av_context_ = avcodec_alloc_context3(av_codec_);

    av_context_->sample_rate = sample_rate;
    av_context_->channels = static_cast<int>(channels);
    av_context_->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;

    if (avcodec_open2(av_context_, av_codec_, nullptr) < 0) {
      REX_FAIL(apu, -1, "XMAContext: Failed to reopen FFmpeg context");
    }
    return 1;
  }
  return 0;
}

void XMAContext::PreparePacket(u32 frame_size, u32 frame_padding) {
  av_packet_->data = xma_frame_.data();
  av_packet_->size = static_cast<int>(1 + ((frame_padding + frame_size) / 8) +
                                      (((frame_padding + frame_size) % 8) ? 1 : 0));

  auto padding_end = av_packet_->size * 8 - (8 + frame_padding + frame_size);
  REX_ASSERT(padding_end < 8);
  xma_frame_[0] = ((frame_padding & 7) << 5) | ((padding_end & 7) << 2);
}

bool XMAContext::DecodePacket(AVCodecContext* av_context, const AVPacket* av_packet,
                              AVFrame* av_frame) const {
  auto ret = avcodec_send_packet(av_context, av_packet);
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    REX_FAIL(apu, false, "XMAContext {}: Error sending packet for decoding, {} ({})", GetID(),
             errbuf, ret);
  }
  ret = avcodec_receive_frame(av_context, av_frame);

  if (ret == AVERROR(EAGAIN)) {
    return false;
  }
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    REX_FAIL(apu, false, "XMAContext {}: Error during decoding, {} ({})", GetID(), errbuf, ret);
  }
  return true;
}

void XMAContext::Decode(XMA_CONTEXT_DATA* data) {
  REX_PROFILE_SCOPE("apu");

  if (!data->IsAnyInputBufferValid()) {
    return;
  }

  if (current_frame_remaining_subframes_ > 0) {
    return;
  }

  if (!data->IsCurrentInputBufferValid()) {
    SwapInputBuffer(data);
    if (!data->IsCurrentInputBufferValid()) {
      return;
    }
  }

  u8* current_input_buffer = GetCurrentInputBuffer(data);

  input_buffer_.fill(0);

  bool is_loop_end_frame = false;
  if (data->loop_count > 0) {
    const u32 loop_end = std::max(kBitsPerPacketHeader, data->loop_end);
    is_loop_end_frame = (data->input_buffer_read_offset == loop_end);
  }

  if (!data->output_buffer_block_count) {
    REXLOG_ERROR(apu, "XMAContext {}: Received 0 for output_buffer_block_count!", GetID());
    return;
  }

  if (data->input_buffer_read_offset < kBitsPerPacketHeader) {
    data->input_buffer_read_offset = kBitsPerPacketHeader;
  }

  const u32 current_input_size = GetCurrentInputBufferSize(data);
  const u32 current_input_packet_count = current_input_size / kBytesPerPacket;

  const i16 packet_index = GetPacketNumber(current_input_size, data->input_buffer_read_offset);

  if (packet_index == -1) {
    REXLOG_ERROR(apu, "XMAContext {}: Invalid packet index, input read offset {}", GetID(),
                 static_cast<u32>(data->input_buffer_read_offset));
    return;
  }

  u8* packet = current_input_buffer + static_cast<size_t>(packet_index) * kBytesPerPacket;
  const u32 packet_first_frame_offset = xma::GetPacketFrameOffset(packet);
  u32 relative_offset = data->input_buffer_read_offset % kBitsPerPacket;

  if (relative_offset < packet_first_frame_offset) {
    data->input_buffer_read_offset = (packet_index * kBitsPerPacket) + packet_first_frame_offset;
    relative_offset = packet_first_frame_offset;
  }

  const u8 skip_count = xma::GetPacketSkipCount(packet);

  if (skip_count == 0xFF) {
    u32 next_input_offset =
        GetNextPacketReadOffset(current_input_buffer, packet_index + 1, current_input_packet_count);
    if (next_input_offset == kBitsPerPacketHeader) {
      SwapInputBuffer(data);
    }
    data->input_buffer_read_offset = next_input_offset;
    return;
  }

  PacketFrame packet_info = GetPacketInfo(packet, relative_offset);
  const u32 packet_to_skip = skip_count + 1;
  const u32 next_packet_index = packet_index + packet_to_skip;

  if (packet_info.current_frame_size == 0) {
    const u8* next_packet = GetNextPacket(data, next_packet_index, current_input_packet_count);
    if (!next_packet) {
      SwapInputBuffer(data);
      return;
    }
    std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);
    std::memcpy(input_buffer_.data() + kBytesPerPacketData, next_packet + kBytesPerPacketHeader,
                kBytesPerPacketData);

    memory::BitStream combined(input_buffer_.data(),
                               size_t{kBitsPerPacket - kBitsPerPacketHeader} * 2);
    combined.SetOffset(relative_offset - kBitsPerPacketHeader);

    u64 frame_size = combined.Peek(kBitsPerFrameHeader);
    if (frame_size == xma::kMaxFrameLength) {
      data->error_status = 4;
      return;
    }
    packet_info.current_frame_size = static_cast<u32>(frame_size);
  }

  memory::BitStream stream(current_input_buffer,
                           (static_cast<size_t>(packet_index) + 1) * kBitsPerPacket);
  stream.SetOffset(data->input_buffer_read_offset);

  const u64 bits_to_copy = GetAmountOfBitsToRead(static_cast<u32>(stream.GetBitsRemaining()),
                                                 packet_info.current_frame_size);

  if (bits_to_copy == 0) {
    REXLOG_ERROR(apu, "XMAContext {}: There are no bits to copy!", GetID());
    SwapInputBuffer(data);
    return;
  }

  if (packet_info.IsLastFrameInPacket()) {
    if (stream.GetBitsRemaining() < packet_info.current_frame_size) {
      const u8* next_packet = GetNextPacket(data, next_packet_index, current_input_packet_count);
      if (!next_packet) {
        data->error_status = 4;
        return;
      }
      std::memcpy(input_buffer_.data() + kBytesPerPacketData, next_packet + kBytesPerPacketHeader,
                  kBytesPerPacketData);
    }
  }

  std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);

  stream =
      memory::BitStream(input_buffer_.data(), size_t{kBitsPerPacket - kBitsPerPacketHeader} * 2);
  stream.SetOffset(relative_offset - kBitsPerPacketHeader);

  xma_frame_.fill(0);

  const u32 padding_start =
      static_cast<u8>(stream.Copy(xma_frame_.data() + 1, packet_info.current_frame_size));

  decoded_frame_.fill(0);

  if (PrepareDecoder(data->sample_rate, static_cast<bool>(data->is_stereo)) == 1) {
    // A reopened codec restarts its output, so the carried tail is stale.
    carry_valid_ = false;
  }
  PreparePacket(packet_info.current_frame_size, padding_start);
  const bool decoded = DecodePacket(av_context_, av_packet_, av_frame_);
  if (decoded) {
    ConvertFrame(reinterpret_cast<const u8**>(&av_frame_->data), static_cast<bool>(data->is_stereo),
                 decoded_frame_.data());

    const size_t pad_bytes = kDecoderStartPadding * kBytesPerSample << data->is_stereo;
    const size_t carry_bytes = (kBytesPerFrameChannel << data->is_stereo) - pad_bytes;

    const u8 decoded_output_limit =
        is_loop_end_frame ? static_cast<u8>((data->loop_subframe_end + 1) << data->is_stereo) : 0;
    const u8 decoded_start_skip =
        loop_start_skip_pending_ ? static_cast<u8>(data->loop_subframe_skip << data->is_stereo) : 0;
    loop_start_skip_pending_ = false;

    if (carry_valid_) {
      std::memcpy(raw_frame_.data(), carry_frame_.data(), carry_bytes);
      std::memcpy(raw_frame_.data() + carry_bytes, decoded_frame_.data(), pad_bytes);

      current_frame_remaining_subframes_ = 4 << data->is_stereo;
      loop_frame_output_limit_ = pending_output_limit_;
      current_frame_remaining_subframes_ -=
          std::min(pending_start_skip_, current_frame_remaining_subframes_);
    }

    std::memcpy(carry_frame_.data(), decoded_frame_.data() + pad_bytes, carry_bytes);
    carry_valid_ = true;
    pending_output_limit_ = decoded_output_limit;
    pending_start_skip_ = decoded_start_skip;
  } else {
    // A skipped frame breaks adjacency, so the carry is discarded.
    carry_valid_ = false;
  }

  if (is_loop_end_frame) {
    UpdateLoopStatus(data);
    return;
  }

  if (!packet_info.IsLastFrameInPacket()) {
    const u32 next_frame_offset = (data->input_buffer_read_offset + bits_to_copy) % kBitsPerPacket;
    data->input_buffer_read_offset = (packet_index * kBitsPerPacket) + next_frame_offset;
    return;
  }

  u32 next_input_offset =
      GetNextPacketReadOffset(current_input_buffer, next_packet_index, current_input_packet_count);

  if (next_input_offset == kBitsPerPacketHeader) {
    SwapInputBuffer(data);
    if (data->IsAnyInputBufferValid()) {
      next_input_offset = xma::GetPacketFrameOffset(
          GetMemory()->TranslatePhysical(data->GetCurrentInputBufferAddress()));

      if (next_input_offset > kMaxFrameSizeInBits) {
        SwapInputBuffer(data);
        return;
      }
    }
  }
  data->input_buffer_read_offset = next_input_offset;
}

void XMAContext::ConvertFrame(const u8** samples, bool is_two_channel, u8* output_buffer) {
  constexpr f32 kScale = (1 << 15) - 1;
  auto out = reinterpret_cast<i16*>(output_buffer);

  static_assert(kSamplesPerFrame % 8 == 0);
  const auto in_channel_0 = reinterpret_cast<const f32*>(samples[0]);
  const simde__m128 scale_mm = simde_mm_set1_ps(kScale);
  if (is_two_channel) {
    const auto in_channel_1 = reinterpret_cast<const f32*>(samples[1]);
    const simde__m128i shufmask =
        simde_mm_set_epi8(14, 15, 6, 7, 12, 13, 4, 5, 10, 11, 2, 3, 8, 9, 0, 1);
    for (u32 i = 0; i < kSamplesPerFrame; i += 4) {
      simde__m128 in_mm0 = simde_mm_loadu_ps(&in_channel_0[i]);
      simde__m128 in_mm1 = simde_mm_loadu_ps(&in_channel_1[i]);
      in_mm0 = simde_mm_mul_ps(in_mm0, scale_mm);
      in_mm1 = simde_mm_mul_ps(in_mm1, scale_mm);
      simde__m128i out_mm0 = simde_mm_cvtps_epi32(in_mm0);
      simde__m128i out_mm1 = simde_mm_cvtps_epi32(in_mm1);
      simde__m128i out_mm = simde_mm_packs_epi32(out_mm0, out_mm1);
      out_mm = simde_mm_shuffle_epi8(out_mm, shufmask);
      simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(&out[size_t{i} * 2]), out_mm);
    }
  } else {
    const simde__m128i shufmask =
        simde_mm_set_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);
    for (u32 i = 0; i < kSamplesPerFrame; i += 8) {
      simde__m128 in_mm0 = simde_mm_loadu_ps(&in_channel_0[i]);
      simde__m128 in_mm1 = simde_mm_loadu_ps(&in_channel_0[i + 4]);
      in_mm0 = simde_mm_mul_ps(in_mm0, scale_mm);
      in_mm1 = simde_mm_mul_ps(in_mm1, scale_mm);
      simde__m128i out_mm0 = simde_mm_cvtps_epi32(in_mm0);
      simde__m128i out_mm1 = simde_mm_cvtps_epi32(in_mm1);
      simde__m128i out_mm = simde_mm_packs_epi32(out_mm0, out_mm1);
      out_mm = simde_mm_shuffle_epi8(out_mm, shufmask);
      simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(&out[i]), out_mm);
    }
  }
}

}  // namespace rex::kernel
