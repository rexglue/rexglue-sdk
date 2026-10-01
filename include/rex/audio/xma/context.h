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

#pragma once

#include <array>
#include <atomic>
#include <mutex>

#include <rex/kernel.h>
#include <rex/memory.h>
#include <rex/platform/thread.h>

// XMA audio format:
// From research, XMA appears to be based on WMA Pro with
// a few (very slight) modifications.
// XMA2 is fully backwards-compatible with XMA1.

// Helpful resources:
// https://github.com/koolkdev/libertyv/blob/master/libav_wrapper/xma2dec.c
// https://hcs64.com/mboard/forum.php?showthread=14818
// https://github.com/hrydgard/minidx9/blob/master/Include/xma2defs.h

// Forward declarations
struct AVCodec;
struct AVCodecParserContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace rex::audio {

// This is stored in guest space in big-endian order.
// We load and swap the whole thing to splat here so that we can
// use bitfields.
// This could be important:
// https://www.fmod.org/questions/question/forum-15859
// Appears to be dumped in order (for the most part)

struct XMA_CONTEXT_DATA {
  // DWORD 0
  uint32_t input_buffer_0_packet_count : 12;  // XMASetInputBuffer0, number of
                                              // 2KB packets. Max 4095 packets.
                                              // These packets form a block.
  uint32_t loop_count : 8;                    // +12bit, XMASetLoopData NumLoops
  uint32_t input_buffer_0_valid : 1;          // +20bit, XMAIsInputBuffer0Valid
  uint32_t input_buffer_1_valid : 1;          // +21bit, XMAIsInputBuffer1Valid
  uint32_t output_buffer_block_count : 5;     // +22bit SizeWrite 256byte blocks
  uint32_t output_buffer_write_offset : 5;    // +27bit
                                              // XMAGetOutputBufferWriteOffset
                                              // AKA OffsetWrite

  // DWORD 1
  uint32_t input_buffer_1_packet_count : 12;  // XMASetInputBuffer1, number of
                                              // 2KB packets. Max 4095 packets.
                                              // These packets form a block.
  uint32_t loop_subframe_end : 2;             // +12bit, XMAPlaybackSetLoop
                                              // dwLoopSubframeEnd: last loop frame
                                              // plays subframes 0..end
  uint32_t unk_dword_1_a : 3;                 // +14bit
  uint32_t loop_subframe_skip : 3;            // +17bit, XMAPlaybackSetLoop
                                              // dwLoopSubframeSkip: subframes to
                                              // discard at loop start; 4 = whole
                                              // warm-up frame (frame-aligned loops)
  uint32_t subframe_decode_count : 4;         // +20bit
  uint32_t output_buffer_padding : 3;         // +24bit, extra output buffer blocks
                                              // reserved per decoded frame
  uint32_t sample_rate : 2;                   // +27bit enum of sample rates
  uint32_t is_stereo : 1;                     // +29bit
  uint32_t unk_dword_1_c : 1;                 // +30bit
  uint32_t output_buffer_valid : 1;           // +31bit, XMAIsOutputBufferValid

  // DWORD 2
  uint32_t input_buffer_read_offset : 26;  // XMAGetInputBufferReadOffset
  uint32_t error_status : 5;               // ErrorStatus
  uint32_t error_set : 1;                  // ErrorSet

  // DWORD 3
  uint32_t loop_start : 26;          // XMASetLoopData LoopStartOffset
                                     // frame offset in bits
  uint32_t parser_error_status : 5;  // ParserErrorStatus
  uint32_t parser_error_set : 1;     // ParserErrorSet

  // DWORD 4
  uint32_t loop_end : 26;        // XMASetLoopData LoopEndOffset
                                 // frame offset in bits
  uint32_t packet_metadata : 5;  // XMAGetPacketMetadata
  uint32_t current_buffer : 1;   // ?

  // DWORD 5
  uint32_t input_buffer_0_ptr;  // physical address
  // DWORD 6
  uint32_t input_buffer_1_ptr;  // physical address
  // DWORD 7
  uint32_t output_buffer_ptr;  // physical address
  // DWORD 8
  uint32_t work_buffer_ptr;  // PtrOverlapAdd(?)

  // DWORD 9
  // +0bit, XMAGetOutputBufferReadOffset AKA WriteBufferOffsetRead
  uint32_t output_buffer_read_offset : 5;
  uint32_t : 25;
  uint32_t stop_when_done : 1;       // +30bit
  uint32_t interrupt_when_done : 1;  // +31bit

  // DWORD 10-15
  uint32_t unk_dwords_10_15[6];  // reserved?

  explicit XMA_CONTEXT_DATA(const void* ptr) {
    memory::copy_and_swap(reinterpret_cast<uint32_t*>(this), reinterpret_cast<const uint32_t*>(ptr),
                          sizeof(XMA_CONTEXT_DATA) / 4);
  }

  void Store(void* ptr) {
    memory::copy_and_swap(reinterpret_cast<uint32_t*>(ptr), reinterpret_cast<const uint32_t*>(this),
                          sizeof(XMA_CONTEXT_DATA) / 4);
  }

  bool IsInputBufferValid(uint8_t buffer_index) const {
    return buffer_index == 0 ? input_buffer_0_valid : input_buffer_1_valid;
  }

  bool IsCurrentInputBufferValid() const { return IsInputBufferValid(current_buffer); }

  bool IsAnyInputBufferValid() const { return input_buffer_0_valid || input_buffer_1_valid; }

  uint32_t GetInputBufferAddress(uint8_t buffer_index) const {
    return buffer_index == 0 ? input_buffer_0_ptr : input_buffer_1_ptr;
  }

  uint32_t GetCurrentInputBufferAddress() const { return GetInputBufferAddress(current_buffer); }

  uint32_t GetInputBufferPacketCount(uint8_t buffer_index) const {
    return buffer_index == 0 ? input_buffer_0_packet_count : input_buffer_1_packet_count;
  }

  uint32_t GetCurrentInputBufferPacketCount() const {
    return GetInputBufferPacketCount(current_buffer);
  }

  bool IsConsumeOnlyContext() const {
    return (input_buffer_0_packet_count | input_buffer_1_packet_count) == 0;
  }
};
static_assert_size(XMA_CONTEXT_DATA, 64);

#pragma pack(push, 1)
// XMA2WAVEFORMATEX
struct Xma2ExtraData {
  uint8_t raw[34];
};
static_assert_size(Xma2ExtraData, 34);
#pragma pack(pop)

struct kPacketInfo {
  uint8_t frame_count_ = 0;
  uint8_t current_frame_ = 0;
  uint32_t current_frame_size_ = 0;

  bool isLastFrameInPacket() const {
    return frame_count_ == 0 || current_frame_ == frame_count_ - 1;
  }
};

static constexpr int kIdToSampleRate[4] = {24000, 32000, 44100, 48000};

class XmaContext {
 public:
  static const uint32_t kBytesPerPacket = 2048;
  static const uint32_t kBitsPerPacket = kBytesPerPacket * 8;
  static const uint32_t kBitsPerPacketHeader = 32;
  static const uint32_t kBitsPerFrameHeader = 15;
  static const uint32_t kBytesPerPacketHeader = 4;
  static const uint32_t kBytesPerPacketData = kBytesPerPacket - kBytesPerPacketHeader;

  static const uint32_t kBytesPerSample = 2;
  static const uint32_t kSamplesPerFrame = 512;
  static const uint32_t kSamplesPerSubframe = 128;
  static const uint32_t kBytesPerFrameChannel = kSamplesPerFrame * kBytesPerSample;
  static const uint32_t kBytesPerSubframeChannel = kSamplesPerSubframe * kBytesPerSample;

  static const uint32_t kOutputBytesPerBlock = 256;
  static const uint32_t kOutputMaxSizeBytes = 31 * kOutputBytesPerBlock;
  static const uint32_t kMaxFrameSizeinBits = 0x4000 - kBitsPerPacketHeader;

  // The bitstream declares a start padding that the hardware discards. FFmpeg's
  // xmaframes decoder parses that field and drops it on the floor
  // (wmaprodec.c decode_frame, "start skip"), emitting the padding as ordinary
  // output, so its sample numbering runs this far ahead of the numbering the
  // guest's loop offsets use. Measured at 192 on every wave checked. Left
  // uncorrected it costs each loop wrap its final 192 samples.
  static const uint32_t kDecoderStartPadding = 192;
  static_assert(kDecoderStartPadding < kSamplesPerFrame,
                "start padding must fit inside one decoded frame");

  explicit XmaContext();
  ~XmaContext();

  int Setup(uint32_t id, runtime::Memory* memory, uint32_t guest_ptr);
  bool Work();

  void Enable();
  bool Block(bool poll);
  void Clear();
  void Disable();
  void Release();

  runtime::Memory* memory() const { return memory_; }

  uint32_t id() { return id_; }
  uint32_t guest_ptr() { return guest_ptr_; }
  bool is_allocated() { return is_allocated_.load(std::memory_order_acquire); }
  bool is_enabled() { return is_enabled_.load(std::memory_order_acquire); }

  void set_is_allocated(bool is_allocated) {
    is_allocated_.store(is_allocated, std::memory_order_release);
  }
  void set_is_enabled(bool is_enabled) { is_enabled_.store(is_enabled, std::memory_order_release); }

  void SignalWorkDone() {
    if (work_completion_event_) {
      work_completion_event_->Set();
    }
  }
  void WaitForWorkDone() {
    if (work_completion_event_) {
      rex::platform::Wait(work_completion_event_.get(), false);
    }
  }

 private:
  static void SwapInputBuffer(XMA_CONTEXT_DATA* data);
  static int GetSampleRate(int id);
  static int16_t GetPacketNumber(size_t size, size_t bit_offset);
  static uint32_t GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data);

  kPacketInfo GetPacketInfo(uint8_t* packet, uint32_t frame_offset);
  uint32_t GetAmountOfBitsToRead(uint32_t remaining_stream_bits, uint32_t frame_size);
  const uint8_t* GetNextPacket(XMA_CONTEXT_DATA* data, uint32_t next_packet_index,
                               uint32_t current_input_packet_count);
  uint32_t GetNextPacketReadOffset(uint8_t* buffer, uint32_t next_packet_index,
                                   uint32_t current_input_packet_count);
  uint8_t* GetCurrentInputBuffer(XMA_CONTEXT_DATA* data);

  void Decode(XMA_CONTEXT_DATA* data);
  void Consume(memory::RingBuffer* output_rb, const XMA_CONTEXT_DATA* data);
  void UpdateLoopStatus(XMA_CONTEXT_DATA* data);
  void ClearLocked(XMA_CONTEXT_DATA* data);
  void ResetDecoderState();

  memory::RingBuffer PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data);
  int PrepareDecoder(int sample_rate, bool is_two_channel);
  void PreparePacket(uint32_t frame_size, uint32_t frame_padding);
  bool DecodePacket(AVCodecContext* av_context, const AVPacket* av_packet, AVFrame* av_frame);

  void StoreContextMerged(const XMA_CONTEXT_DATA& data, const XMA_CONTEXT_DATA& initial_data,
                          uint8_t* context_ptr);

  static void ConvertFrame(const uint8_t** samples, bool is_two_channel, uint8_t* output_buffer);

  runtime::Memory* memory_ = nullptr;
  std::unique_ptr<rex::platform::Event> work_completion_event_;

  uint32_t id_ = 0;
  uint32_t guest_ptr_ = 0;
  std::mutex lock_;
  std::atomic<bool> is_allocated_ = false;
  std::atomic<bool> is_enabled_ = false;

  // ffmpeg structures
  AVPacket* av_packet_ = nullptr;
  AVCodec* av_codec_ = nullptr;
  AVCodecContext* av_context_ = nullptr;
  AVFrame* av_frame_ = nullptr;

  // Packet data buffer (two packets worth for split frame handling)
  std::array<uint8_t, kBytesPerPacketData * 2> input_buffer_;
  // First byte contains bit offset information
  std::array<uint8_t, 1 + 4096> xma_frame_;
  // Conversion buffer for up to 2-channel frame
  std::array<uint8_t, kBytesPerFrameChannel * 2> raw_frame_;
  // Freshly decoded block, before start-padding realignment
  std::array<uint8_t, kBytesPerFrameChannel * 2> decoded_frame_;
  // Tail of the previous decoded block, awaiting the next block's padding head
  std::array<uint8_t, kBytesPerFrameChannel * 2> carry_frame_;

  // Output buffer tracking
  int32_t remaining_subframe_blocks_in_output_buffer_ = 0;
  uint8_t current_frame_remaining_subframes_ = 0;

  // Loop subframe precision state
  uint8_t loop_frame_output_limit_ = 0;
  bool loop_start_skip_pending_ = false;

  // Start-padding realignment state. Attributes belong to the frame whose
  // samples are still being assembled, so they land one decode after the frame
  // they describe.
  bool carry_valid_ = false;
  uint8_t pending_output_limit_ = 0;
  uint8_t pending_start_skip_ = 0;
};

}  // namespace rex::audio
