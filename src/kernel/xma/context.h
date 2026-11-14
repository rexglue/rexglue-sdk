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

#include <rex/kernel/heap.h>
#include <rex/memory.h>
#include <rex/platform/thread.h>
#include <rex/platform/sync.h>

struct AVCodec;
struct AVCodecParserContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace rex::kernel {

/**
 * XMA context as laid out in physical memory, 64 bytes of big endian bitfields.
 * Loaded byte swapped so the bitfields apply.
 */
struct XMA_CONTEXT_DATA {
  // DWORD 0
  u32 input_buffer_0_packet_count : 12; /**< XMASetInputBuffer0, 2KB packets, max 4095 */
  u32 loop_count : 8;
  u32 input_buffer_0_valid : 1;
  u32 input_buffer_1_valid : 1;
  u32 output_buffer_block_count : 5; /**< SizeWrite in 256 byte blocks */
  /** XMAGetOutputBufferWriteOffset (OffsetWrite) */
  u32 output_buffer_write_offset : 5;

  // DWORD 1
  u32 input_buffer_1_packet_count : 12; /**< XMASetInputBuffer1, 2KB packets, max 4095 */
  /** XMAPlaybackSetLoop dwLoopSubframeEnd, the last loop frame plays subframes 0..end */
  u32 loop_subframe_end : 2;
  u32 unk_dword_1_a : 3;
  /**
   * XMAPlaybackSetLoop dwLoopSubframeSkip, subframes discarded at loop start.
   * 4 means a whole warm up frame (frame aligned loops).
   */
  u32 loop_subframe_skip : 3;
  u32 subframe_decode_count : 4;
  /** Extra output blocks reserved per decoded frame */
  u32 output_buffer_padding : 3;
  u32 sample_rate : 2; /**< Index into kIdToSampleRate */
  u32 is_stereo : 1;
  u32 unk_dword_1_c : 1;
  u32 output_buffer_valid : 1;

  // DWORD 2
  u32 input_buffer_read_offset : 26; /**< XMAGetInputBufferReadOffset */
  u32 error_status : 5;
  u32 error_set : 1;

  // DWORD 3
  u32 loop_start : 26; /**< XMASetLoopData LoopStartOffset, frame offset in bits */
  u32 parser_error_status : 5;
  u32 parser_error_set : 1;

  // DWORD 4
  u32 loop_end : 26;       /**< XMASetLoopData LoopEndOffset, frame offset in bits */
  u32 packet_metadata : 5; /**< XMAGetPacketMetadata */
  u32 current_buffer : 1;

  // DWORD 5
  u32 input_buffer_0_ptr; /**< Physical address */
  // DWORD 6
  u32 input_buffer_1_ptr; /**< Physical address */
  // DWORD 7
  u32 output_buffer_ptr; /**< Physical address */
  // DWORD 8
  u32 work_buffer_ptr;

  // DWORD 9
  /** XMAGetOutputBufferReadOffset (WriteBufferOffsetRead) */
  u32 output_buffer_read_offset : 5;
  u32 : 25;
  u32 stop_when_done : 1;
  u32 interrupt_when_done : 1;

  u32 unk_dwords_10_15[6];

  explicit XMA_CONTEXT_DATA(const void* ptr) {
    memory::copy_and_swap(reinterpret_cast<u32*>(this), reinterpret_cast<const u32*>(ptr),
                          sizeof(XMA_CONTEXT_DATA) / 4);
  }

  void Store(void* ptr) {
    memory::copy_and_swap(reinterpret_cast<u32*>(ptr), reinterpret_cast<const u32*>(this),
                          sizeof(XMA_CONTEXT_DATA) / 4);
  }

  bool IsInputBufferValid(u8 buffer_index) const {
    return buffer_index == 0 ? input_buffer_0_valid : input_buffer_1_valid;
  }

  bool IsCurrentInputBufferValid() const { return IsInputBufferValid(current_buffer); }

  bool IsAnyInputBufferValid() const { return input_buffer_0_valid || input_buffer_1_valid; }

  u32 GetInputBufferAddress(u8 buffer_index) const {
    return buffer_index == 0 ? input_buffer_0_ptr : input_buffer_1_ptr;
  }

  u32 GetCurrentInputBufferAddress() const { return GetInputBufferAddress(current_buffer); }

  u32 GetInputBufferPacketCount(u8 buffer_index) const {
    return buffer_index == 0 ? input_buffer_0_packet_count : input_buffer_1_packet_count;
  }

  u32 GetCurrentInputBufferPacketCount() const { return GetInputBufferPacketCount(current_buffer); }

  bool IsConsumeOnlyContext() const {
    return (input_buffer_0_packet_count | input_buffer_1_packet_count) == 0;
  }
};
REX_STATIC_ASSERT_SIZE(XMA_CONTEXT_DATA, 64);

struct PacketFrame {
  u8 frame_count = 0;
  u8 current_frame = 0;
  u32 current_frame_size = 0;

  bool IsLastFrameInPacket() const { return frame_count == 0 || current_frame == frame_count - 1; }
};

static constexpr int kIdToSampleRate[4] = {24000, 32000, 44100, 48000};

/**
 * Decodes the stream of one XMA context.
 * XMA is based on WMA Pro and XMA2 is backward compatible with XMA1.
 */
class XMAContext {
 public:
  static const u32 kBytesPerPacket = 2048;
  static const u32 kBitsPerPacket = kBytesPerPacket * 8;
  static const u32 kBitsPerPacketHeader = 32;
  static const u32 kBitsPerFrameHeader = 15;
  static const u32 kBytesPerPacketHeader = 4;
  static const u32 kBytesPerPacketData = kBytesPerPacket - kBytesPerPacketHeader;

  static const u32 kBytesPerSample = 2;
  static const u32 kSamplesPerFrame = 512;
  static const u32 kSamplesPerSubframe = 128;
  static const u32 kBytesPerFrameChannel = kSamplesPerFrame * kBytesPerSample;
  static const u32 kBytesPerSubframeChannel = kSamplesPerSubframe * kBytesPerSample;

  static const u32 kOutputBytesPerBlock = 256;
  static const u32 kOutputMaxSizeBytes = 31 * kOutputBytesPerBlock;
  static const u32 kMaxFrameSizeInBits = 0x4000 - kBitsPerPacketHeader;

  /**
   * Start padding the bitstream declares, which the xmaframes decoder emits as output.
   * Uncorrected, each loop wrap loses its final 192 samples.
   */
  static const u32 kDecoderStartPadding = 192;
  static_assert(kDecoderStartPadding < kSamplesPerFrame,
                "start padding must fit inside 1 decoded frame");

  explicit XMAContext();
  ~XMAContext();

  int Setup(u32 id, kernel::GuestAddressSpace* memory, u32 guest_ptr);
  bool Work();

  void Enable();
  bool WaitIdle(bool poll);
  void Clear();
  void Disable();
  void Release();

  kernel::GuestAddressSpace* GetAddressSpace() const { return memory_; }

  u32 GetID() const { return id_; }
  u32 GetGuestPtr() const { return guest_ptr_; }
  bool IsAllocated() { return is_allocated_.load(std::memory_order_acquire); }
  bool IsEnabled() { return is_enabled_.load(std::memory_order_acquire); }

  void SetAllocated(bool is_allocated) {
    is_allocated_.store(is_allocated, std::memory_order_release);
  }
  void SetEnabled(bool is_enabled) { is_enabled_.store(is_enabled, std::memory_order_release); }

  void SignalWorkDone() {
    if (work_completion_event_) {
      work_completion_event_->Set();
    }
  }

 private:
  static void SwapInputBuffer(XMA_CONTEXT_DATA* data);
  static int GetSampleRate(int id);
  static i16 GetPacketNumber(size_t size, size_t bit_offset);
  static u32 GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data);

  PacketFrame GetPacketInfo(u8* packet, u32 frame_offset);
  u32 GetAmountOfBitsToRead(u32 remaining_stream_bits, u32 frame_size);
  const u8* GetNextPacket(XMA_CONTEXT_DATA* data, u32 next_packet_index,
                          u32 current_input_packet_count) const;
  u32 GetNextPacketReadOffset(u8* buffer, u32 next_packet_index, u32 current_input_packet_count);
  u8* GetCurrentInputBuffer(XMA_CONTEXT_DATA* data) const;

  /**
   * Decodes the stream. The decoder emits the start padding as output, so each block is realigned
   * by carrying the tail of the previous decoded block into the head of the next one.
   *
   * @param data The context data.
   */
  void Decode(XMA_CONTEXT_DATA* data);
  void Consume(memory::RingBuffer* output_rb, const XMA_CONTEXT_DATA* data);
  void UpdateLoopStatus(XMA_CONTEXT_DATA* data);
  void ClearLocked(XMA_CONTEXT_DATA* data);
  /**
   * Discards the MDCT overlap add tail so a freed or reinitialized context starts a new stream.
   * A flush does not clear it because the xmaframes decoder declares no flush callback.
   */
  void ResetDecoderState();

  memory::RingBuffer PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data);
  int PrepareDecoder(int sample_rate, bool is_two_channel);
  void PreparePacket(u32 frame_size, u32 frame_padding);
  bool DecodePacket(AVCodecContext* av_context, const AVPacket* av_packet, AVFrame* av_frame) const;

  void StoreContextMerged(const XMA_CONTEXT_DATA& data, const XMA_CONTEXT_DATA& initial_data,
                          u8* context_ptr);

  /**
   * Converts decoded float samples to byte swapped int16, interleaving two channels.
   * Output saturates because decoder samples can exceed [-1, 1].
   *
   * @param samples The decoded channel sample pointers.
   * @param is_two_channel True when the frame has two channels.
   * @param output_buffer Receives the int16 samples.
   */
  static void ConvertFrame(const u8** samples, bool is_two_channel, u8* output_buffer);

  kernel::GuestAddressSpace* memory_ = nullptr;
  std::unique_ptr<rex::platform::Event> work_completion_event_;

  u32 id_ = 0;
  u32 guest_ptr_ = 0;
  std::mutex lock_;
  std::atomic<bool> is_allocated_ = false;
  std::atomic<bool> is_enabled_ = false;

  AVPacket* av_packet_ = nullptr;
  AVCodec* av_codec_ = nullptr;
  AVCodecContext* av_context_ = nullptr;
  AVFrame* av_frame_ = nullptr;

  /** Holds two packets of data for split frame handling. */
  std::array<u8, size_t{kBytesPerPacketData} * 2> input_buffer_;
  /** First byte holds the bit offset */
  std::array<u8, 1 + 4096> xma_frame_;
  /** Realigned output frame built from carry_frame_ and decoded_frame_, up to two channels. */
  std::array<u8, size_t{kBytesPerFrameChannel} * 2> raw_frame_;
  /** Freshly decoded block, before start padding realignment */
  std::array<u8, size_t{kBytesPerFrameChannel} * 2> decoded_frame_;
  /** Tail of the previous decoded block, awaiting the next block padding head */
  std::array<u8, size_t{kBytesPerFrameChannel} * 2> carry_frame_;

  i32 remaining_subframe_blocks_in_output_buffer_ = 0;
  u8 current_frame_remaining_subframes_ = 0;

  u8 loop_frame_output_limit_ = 0;
  bool loop_start_skip_pending_ = false;

  bool carry_valid_ = false;
  /**
   * Pending limit and skip belong to the frame being assembled,
   * so they apply one decode after the frame they belong to.
   */
  u8 pending_output_limit_ = 0;
  u8 pending_start_skip_ = 0;
};

}  // namespace rex::kernel
