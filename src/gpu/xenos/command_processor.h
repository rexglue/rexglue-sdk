/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <atomic>
#include <bit>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/logging/log_category.h>
#include <rex/math.h>
#include <rex/setting.h>
#include <rex/types.h>
#include <rex/xenos/register.h>
#include <rex/xenos/format.h>
#include <rex/xenos/packet.h>
#include <rex/xenos/state.h>
#include <rex/memory.h>
#include <rex/memory/stream.h>
#include <rex/guest.h>
#include <rex/title.h>
#include <rex/platform/thread.h>
#include <rex/utility.h>
#include <rex/platform/sync.h>
#include "gpu/xenos/host/presenter.h"
#include "gpu/xenos/xenos_export.h"

namespace rex::gpu {

class XenosGraphicsDriverBase;
class XenosSettings;
class Shader;

enum class ReadbackResolveMode : u8 {
  kDisabled,
  kFast,
  kSome,
  kFull,
};

struct SwapState {
  // Lock must be held when changing data in this structure.
  std::mutex mutex;
  u32 width = 0;
  u32 height = 0;
  uintptr_t front_buffer_texture = 0;
  uintptr_t back_buffer_texture = 0;
  void* driver_data = nullptr;
  bool pending = false;
};

enum class SwapMode : u8 {
  kNormal,
  kIgnored,
};

enum class GammaRampType : u8 {
  kUnknown = 0,
  kTable,
  kPWL,
};

class REX_GPU_XENOS_API CommandProcessor : public rex::NonCopyable {
 public:
  enum class SwapPostEffect : u8 {
    kNone,
    kFXAA,
    kFXAAExtreme,
  };

  CommandProcessor(XenosGraphicsDriverBase* graphics_system, const TitleInfo& info,
                   IPhysicalMemory& memory, IPhysicalWriteWatch& watch,
                   IGuestInterrupts& interrupts);

  const XenosSettings& GetXenosSettings() const;

  XenosGraphicsDriverBase& GetGraphicsDriver() const { return *graphics_system_; }

  virtual ~CommandProcessor();

  u32 GetCounter() const { return counter_; }
  void IncrementCounter() {
    CallInThread([this] { counter_++; });
  }

  Shader* GetActiveVertexShader() const { return active_vertex_shader_; }
  Shader* GetActivePixelShader() const { return active_pixel_shader_; }

  /** Initializes the gamma ramps to the linear values titles set when starting with sRGB gamma. */
  virtual bool Initialize();
  virtual void Shutdown();

  void CallInThread(rex::MoveOnlyFunction<void()> fn);

  /**
   * Runs a function on the worker and waits for it. It runs on the calling thread when that is the
   * worker or the worker is not running.
   */
  void CallInThreadAndWait(rex::MoveOnlyFunction<void()> fn);

  virtual void ClearCaches();
  virtual void InvalidateGPUMemory();

  // "Desired" is for the external thread managing the post processing effect.
  void SetDesiredSwapPostEffect(SwapPostEffect swap_post_effect);
  /**
   * Issues a swap. The front buffer may be a texture written by the CPU, not a resolve destination.
   */
  virtual void IssueSwap(u32 frontbuffer_ptr, u32 frontbuffer_width, u32 frontbuffer_height) = 0;

  /**
   * Initializes the shader storage. May run on another thread while the command processor is
   * paused.
   *
   * @param blocking True to wait for the storage to finish loading.
   */
  virtual void InitializeShaderStorage(const std::filesystem::path& cache_root, u32 title_id,
                                       bool blocking);

  /**
   * Sets the primary ring buffer. The worker applies it before it reads the next packet, so the
   * read index is 0 once it returns to its loop, and the call does not wait for the worker.
   *
   * @param ptr The physical address of the buffer.
   * @param size_log2 The log2 of the buffer size in quadwords.
   */
  void InitializeRingBuffer(u32 ptr, u32 size_log2);
  /**
   * Enables writing the read pointer back to memory. The worker applies it at its next loop, and
   * the call does not wait for the worker.
   *
   * @param ptr RB_RPTR_ADDR of CP_RB_RPTR_ADDR (0x70C), the address to write the read pointer to.
   * @param block_size_log2 RB_BLKSZ of CP_RB_CNTL (0x704), log2 of quadwords read between updates.
   */
  void EnableReadPointerWriteBack(u32 ptr, u32 block_size_log2);

  void UpdateWritePointer(u32 value);

  void ExecutePacket(u32 ptr, u32 count);

 protected:
  /** Value in an occlusion query result that marks it finished. */
  static constexpr u32 kQueryFinished = std::byteswap(0xFFFFFEEDu);

  static constexpr u32 kReadbackBufferSizeIncrement = 16 * 1024 * 1024;
  static constexpr size_t kMaxReadbackBuffers = 256;
  static constexpr u64 kReadbackBufferEvictionAgeFrames = 60;
  static constexpr u32 kMaxOcclusionQueries = 8192;
  static constexpr u32 kVertexFetchCount = 96;

  static u32 AlignReadbackBufferSize(u32 size) {
    if (size < 1 * 1024 * 1024) {
      return rex::RoundUpPow2(size, 256u * 1024u);
    }
    if (size < 4 * 1024 * 1024) {
      return rex::RoundUpPow2(size, 1u * 1024u * 1024u);
    }
    return rex::RoundUpPow2(size, kReadbackBufferSizeIncrement);
  }
  static u64 BuildReadbackResolveKey(u32 address, u32 length) {
    return (static_cast<u64>(address) << 32) | static_cast<u64>(length);
  }
  static u64 BuildMemexportReadbackKey(u32 first_base_address_dwords, u32 total_size) {
    return (static_cast<u64>(first_base_address_dwords) << 32) | static_cast<u64>(total_size);
  }

  /**
   * Takes the next host occlusion query index, wrapping at kMaxOcclusionQueries.
   *
   * @param host_index_out Receives the index.
   * @return Always true.
   */
  bool AcquireOcclusionQueryIndex(u32& host_index_out);

  /** Marks every vertex fetch constant as needing a new upload. */
  void InvalidateAllVertexBufferResidency();

  /** Marks one vertex fetch constant as needing a new upload. */
  void InvalidateVertexBufferResidency(u32 vfetch_index);

  /**
   * Marks a range of vertex fetch constants as needing a new upload.
   *
   * @param first_vfetch The first index, swapped with last_vfetch when greater.
   */
  void InvalidateVertexBufferResidencyRange(u32 first_vfetch, u32 last_vfetch);

  u32 occlusion_query_cursor_ = 0;
  u64 vertex_buffers_in_sync_[2] = {};

  struct IndexBuffer {
    xenos::IndexFormat format = xenos::IndexFormat::kInt16;
    xenos::Endian endianness = xenos::Endian::kNone;
    u32 count = 0;
    u32 guest_base = 0;
    size_t length = 0;
  };

  void WorkerThreadMain();

  /**
   * Writes an occlusion query result. The fields a title polls are written last.
   *
   * @param samples The passed sample count for Total_A and ZPass_A. Every other field is 0.
   */
  static void WriteSampleCounts(xenos::DepthSampleCounts& counts, u32 samples);
  bool IsWorkerThread();
  /**
   * Runs a function on the worker without waiting for it, so a caller never blocks on a worker that
   * is inside a packet. It runs on the calling thread when that is the worker or the worker is not
   * running.
   */
  void CallInThreadNoWait(rex::MoveOnlyFunction<void()> fn);
  bool HasPendingFns();
  void RunPendingFns();
  virtual bool SetupContext() = 0;
  virtual void ShutdownContext() = 0;

  /**
   * Writes a register. Direct3D 9 sets DC_LUT_RW_INDEX rw_index to 0x80 after the last PWL entry,
   * although the documentation says bit 7 is ignored for PWL.
   */
  virtual void WriteRegister(u32 index, u32 value);
  u32 ReadRegisterValue(u32 index) const;
  virtual void WriteRegistersFromMem(u32 start_index, u32* base, u32 num_registers);
  virtual void WriteRegisterRangeFromRing(memory::RingBuffer* ring, u32 base, u32 num_registers);
  void WriteALURangeFromRing(memory::RingBuffer* ring, u32 base, u32 num_registers);
  void WriteFetchRangeFromRing(memory::RingBuffer* ring, u32 base, u32 num_registers);
  void WriteBoolRangeFromRing(memory::RingBuffer* ring, u32 base, u32 num_registers);
  void WriteLoopRangeFromRing(memory::RingBuffer* ring, u32 base, u32 num_registers);
  void WriteRegisterBlockRangeFromRing(memory::RingBuffer* ring, u32 base, u32 num_registers);
  void WriteALURangeFromMem(u32 start_index, u32* base, u32 num_registers);
  void WriteFetchRangeFromMem(u32 start_index, u32* base, u32 num_registers);
  void WriteBoolRangeFromMem(u32 start_index, u32* base, u32 num_registers);
  void WriteLoopRangeFromMem(u32 start_index, u32* base, u32 num_registers);
  void WriteRegisterBlockRangeFromMem(u32 start_index, u32* base, u32 num_registers);

  const xenos::reg::DC_LUT_30_COLOR* GetGammaRamp256EntryTable() const {
    return gamma_ramp_256_entry_table_;
  }
  const xenos::reg::DC_LUT_PWL_DATA* GetGammaRampPWLRGB() const { return gamma_ramp_pwl_rgb_[0]; }
  virtual void OnGammaRamp256EntryTableValueWritten() {}
  virtual void OnGammaRampPWLValueWritten() {}

  /**
   * Makes host memory coherent. COHER_STATUS_HOST often holds 0x01000000 or 0x03000000. May run
   * from the WAIT_REG_MEM loop.
   */
  virtual void SyncCoherency();
  virtual void PrepareForWait();
  virtual void ReturnFromWait();

  u32 ExecutePrimaryBuffer(u32 start_index, u32 end_index);
  virtual void OnPrimaryBufferEnd() {}
  void ExecuteIndirectBuffer(u32 ptr, u32 length);
  bool ExecutePacket(memory::RingBuffer* reader);
  bool ExecutePacketType0(memory::RingBuffer* reader, u32 packet);
  bool ExecutePacketType1(memory::RingBuffer* reader, u32 packet);
  bool ExecutePacketType2(memory::RingBuffer* reader, u32 packet);
  /**
   * Executes a type 3 packet. When bit 0 (the predicate) is set, a bin check decides whether the
   * packet runs. Predicated swaps are skipped as they are never valid.
   *
   * @return True when the packet executed.
   */
  bool ExecutePacketType3(memory::RingBuffer* reader, u32 packet);
  bool ExecutePacketType3_ME_INIT(memory::RingBuffer* reader, u32 packet, u32 count);
  bool ExecutePacketType3_NOP(memory::RingBuffer* reader, u32 packet, u32 count);
  bool ExecutePacketType3_INTERRUPT(memory::RingBuffer* reader, u32 packet, u32 count);
  /**
   * Executes the swap packet that VdSwap posts. The packet has 63 words and only the first has
   * data.
   *
   * @return True when the packet executed.
   */
  bool ExecutePacketType3_XE_SWAP(memory::RingBuffer* reader, u32 packet, u32 count);
  bool ExecutePacketType3_INDIRECT_BUFFER(memory::RingBuffer* reader, u32 packet, u32 count);
  bool ExecutePacketType3_WAIT_REG_MEM(memory::RingBuffer* reader, u32 packet, u32 count);
  bool ExecutePacketType3_REG_RMW(memory::RingBuffer* reader, u32 packet, u32 count);
  bool ExecutePacketType3_REG_TO_MEM(memory::RingBuffer* reader, u32 packet, u32 count);
  bool ExecutePacketType3_MEM_WRITE(memory::RingBuffer* reader, u32 packet, u32 count);
  bool ExecutePacketType3_COND_WRITE(memory::RingBuffer* reader, u32 packet, u32 count);
  bool ExecutePacketType3_EVENT_WRITE(memory::RingBuffer* reader, u32 packet, u32 count);
  bool ExecutePacketType3_EVENT_WRITE_SHD(memory::RingBuffer* reader, u32 packet, u32 count);
  /**
   * Executes the screen extent event. It reports the xy coordinates affected by the previous draw,
   * faked as the maximum extent.
   *
   * @return True when the packet executed.
   */
  bool ExecutePacketType3_EVENT_WRITE_EXT(memory::RingBuffer* reader, u32 packet, u32 count);
  /**
   * Executes the occlusion query event, sent on query begin and end. A fixed passed sample count
   * is reported.
   *
   * @return True when the packet executed.
   */
  virtual bool ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader, u32 packet,
                                                  u32 count);
  /**
   * Executes a draw. When viz_query_condition is not 0 the draw is conditional on a viz query.
   * Bits 0 to 5 hold the ID issued in PM4_VIZ_QUERY and bit 8 renders conditionally on the query
   * result. A draw the host cannot execute is skipped.
   *
   * @param opcode_name The opcode name for logging.
   * @return True when processing continues.
   */
  bool ExecutePacketType3Draw(memory::RingBuffer* reader, u32 packet, const char* opcode_name,
                              u32 viz_query_condition, u32 count_remaining);
  /**
   * Fetches the index buffer and draws. Direct3D 9 uses it for the kDMA and kAutoIndex sources.
   * The first token is a viz query token.
   *
   * @return True when processing continues.
   */
  bool ExecutePacketType3_DRAW_INDX(memory::RingBuffer* reader, u32 packet, u32 count);
  /**
   * Draws with the indices in the packet. Direct3D 9 uses it for the kAutoIndex source. There is
   * no viz query token.
   *
   * @return True when processing continues.
   */
  bool ExecutePacketType3_DRAW_INDX_2(memory::RingBuffer* reader, u32 packet, u32 count);
  bool ExecutePacketType3_SET_CONSTANT(memory::RingBuffer* reader, u32 packet, u32 count);
  bool ExecutePacketType3_SET_CONSTANT2(memory::RingBuffer* reader, u32 packet, u32 count);
  bool ExecutePacketType3_LOAD_ALU_CONSTANT(memory::RingBuffer* reader, u32 packet, u32 count);
  bool ExecutePacketType3_SET_SHADER_CONSTANTS(memory::RingBuffer* reader, u32 packet, u32 count);
  bool ExecutePacketType3_IM_LOAD(memory::RingBuffer* reader, u32 packet, u32 count);
  bool ExecutePacketType3_IM_LOAD_IMMEDIATE(memory::RingBuffer* reader,

                                            u32 packet, u32 count);
  bool ExecutePacketType3_INVALIDATE_STATE(memory::RingBuffer* reader, u32 packet, u32 count);
  /**
   * Executes the begin and end initiator for viz query extent processing. Beginning a query clears
   * the scan converter state, which the register does not reflect. At the end the result written
   * back to the register is faked as visible.
   *
   * @return True when the packet executed.
   */
  bool ExecutePacketType3_VIZ_QUERY(memory::RingBuffer* reader, u32 packet, u32 count);

  virtual Shader* LoadShader(xenos::ShaderType shader_type, u32 guest_address,
                             const u32* host_address, u32 dword_count) = 0;

  virtual bool IssueDraw(xenos::PrimitiveType prim_type, u32 index_count,
                         IndexBuffer* index_buffer_info, bool major_mode_explicit) = 0;
  virtual bool IssueCopy() = 0;

  SwapPostEffect GetActualSwapPostEffect() const { return swap_post_effect_actual_; }

  ReadbackResolveMode GetReadbackResolveMode(bool legacy_readback_resolve_enabled) const;
  bool IsReadbackMemexportEnabled() const;

  TitleInfo title_info_;
  IPhysicalMemory& physical_memory_;
  IPhysicalWriteWatch& write_watch_;
  IGuestInterrupts& guest_interrupts_;
  XenosGraphicsDriverBase* graphics_system_ = nullptr;
  xenos::RegisterFile* register_file_ = nullptr;
  rex::log::OncePerKey<xenos::RegisterFile::kRegisterCount> unknown_write_once_;

  std::atomic<bool> worker_running_;
  ScopeGuard worker_thread_;
  std::atomic<std::thread::id> worker_thread_id_;

  std::mutex pending_mutex_;
  std::queue<rex::MoveOnlyFunction<void()>> pending_fns_;

  std::vector<u32> me_bin_;

  u32 counter_ = 0;

  u32 primary_buffer_ptr_ = 0;
  u32 primary_buffer_size_ = 0;

  u32 read_ptr_index_ = 0;
  u32 read_ptr_update_freq_ = 0;
  u32 read_ptr_writeback_ptr_ = 0;

  std::unique_ptr<rex::platform::Event> write_ptr_index_event_;
  std::atomic<u32> write_ptr_index_;

  /** Register writes beyond the emulated register file range, kept for dependent packet logic. */
  std::unordered_map<u32, u32> extended_register_values_;

  u64 bin_select_ = 0xFFFFFFFFull;
  u64 bin_mask_ = 0xFFFFFFFFull;

  Shader* active_vertex_shader_ = nullptr;
  Shader* active_pixel_shader_ = nullptr;

  SwapPostEffect swap_post_effect_desired_ = SwapPostEffect::kNone;
  SwapPostEffect swap_post_effect_actual_ = SwapPostEffect::kNone;

  const Setting<bool>* driver_readback_memexport_ = nullptr;

 private:
  xenos::reg::DC_LUT_30_COLOR gamma_ramp_256_entry_table_[256] = {};
  xenos::reg::DC_LUT_PWL_DATA gamma_ramp_pwl_rgb_[128][3] = {};
  u32 gamma_ramp_rw_component_ = 0;
};

}  // namespace rex::gpu
