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

#include <rex/cvar.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/xenos/register_file.h>
#include <rex/xenos/registers.h>
#include <rex/xenos/xenos.h>
#include <rex/memory.h>
#include <rex/memory/ring_buffer.h>
#include <rex/runtime/xthread.h>
#include <rex/platform/thread.h>
#include <rex/move_only_function.h>
#include "gpu/xenos/host/presenter.h"

namespace rex::gpu {

class GraphicsSystem;
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
  void* backend_data = nullptr;
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

class CommandProcessor {
 public:
  enum class SwapPostEffect : u8 {
    kNone,
    kFxaa,
    kFxaaExtreme,
  };

  CommandProcessor(GraphicsSystem* graphics_system, Runtime& runtime);
  virtual ~CommandProcessor();

  u32 counter() const { return counter_; }
  void increment_counter() {
    CallInThread([this] { counter_++; });
  }

  Shader* active_vertex_shader() const { return active_vertex_shader_; }
  Shader* active_pixel_shader() const { return active_pixel_shader_; }

  /**
   * Initializes the gamma ramps to the linear values titles set when starting with sRGB gamma,
   * which VdGetCurrentDisplayGamma reports as 1.
   *
   * @return True when initialization succeeded.
   */
  virtual bool Initialize();
  virtual void Shutdown();

  void CallInThread(rex::MoveOnlyFunction<void()> fn);

  /**
   * Runs a function on the worker and waits for it to finish. The function runs on the calling
   * thread when that is the worker, or when the worker is not running.
   *
   * @param fn The function to run.
   */
  void CallInThreadAndWait(rex::MoveOnlyFunction<void()> fn);

  virtual void ClearCaches();
  virtual void InvalidateGPUMemory();

  // "Desired" is for the external thread managing the post-processing effect.
  void SetDesiredSwapPostEffect(SwapPostEffect swap_post_effect);
  /**
   * Issues a swap. The front buffer is not always a resolve destination and may be a texture
   * written by the CPU or loaded from a file.
   *
   * @param frontbuffer_ptr The front buffer address.
   * @param frontbuffer_width The front buffer width.
   * @param frontbuffer_height The front buffer height.
   */
  virtual void IssueSwap(u32 frontbuffer_ptr, u32 frontbuffer_width, u32 frontbuffer_height) = 0;

  /**
   * Initializes the shader storage. May be called from a thread other than the command processor
   * thread while the command processor is paused, and its termination may be awaited.
   *
   * @param cache_root The cache root directory.
   * @param title_id The title ID.
   * @param blocking True to wait for the storage to finish loading.
   */
  virtual void InitializeShaderStorage(const std::filesystem::path& cache_root, u32 title_id,
                                       bool blocking);

  void InitializeRingBuffer(u32 ptr, u32 size_log2);
  /**
   * Enables writing the read pointer back to memory.
   *
   * @param ptr RB_RPTR_ADDR of CP_RB_RPTR_ADDR (0x70C), the address the read pointer is written to.
   * @param block_size_log2 RB_BLKSZ of CP_RB_CNTL (0x704), log2 of quadwords read between updates.
   */
  void EnableReadPointerWriteBack(u32 ptr, u32 block_size_log2);

  void UpdateWritePointer(u32 value);

  void ExecutePacket(u32 ptr, u32 count);

 protected:
  struct IndexBufferInfo {
    xenos::IndexFormat format = xenos::IndexFormat::kInt16;
    xenos::Endian endianness = xenos::Endian::kNone;
    u32 count = 0;
    u32 guest_base = 0;
    size_t length = 0;
  };

  void WorkerThreadMain();

  /**
   * Writes an occlusion query result with the fields a title polls written last, as release
   * stores.
   *
   * @param counts The guest sample counts.
   * @param samples The passed sample count for Total_A and ZPass_A. Every other field is 0.
   */
  static void WriteSampleCounts(xenos::DepthSampleCounts& counts, u32 samples);
  bool IsWorkerThread();
  bool HasPendingFns();
  void RunPendingFns();
  virtual bool SetupContext() = 0;
  virtual void ShutdownContext() = 0;

  /**
   * Writes a register. Direct3D 9 sets DC_LUT_RW_INDEX rw_index to 0x80 after the last PWL entry
   * although the register documentation says bit 7 is ignored for PWL.
   *
   * @param index The register index.
   * @param value The value to write.
   */
  virtual void WriteRegister(u32 index, u32 value);
  u32 ReadRegisterValue(u32 index) const;
  virtual void WriteRegistersFromMem(u32 start_index, u32* base, u32 num_registers);
  virtual void WriteRegisterRangeFromRing(memory::RingBuffer* ring, u32 base, u32 num_registers);
  void WriteALURangeFromRing(memory::RingBuffer* ring, u32 base, u32 num_registers);
  void WriteFetchRangeFromRing(memory::RingBuffer* ring, u32 base, u32 num_registers);
  void WriteBoolRangeFromRing(memory::RingBuffer* ring, u32 base, u32 num_registers);
  void WriteLoopRangeFromRing(memory::RingBuffer* ring, u32 base, u32 num_registers);
  void WriteREGISTERSRangeFromRing(memory::RingBuffer* ring, u32 base, u32 num_registers);
  void WriteALURangeFromMem(u32 start_index, u32* base, u32 num_registers);
  void WriteFetchRangeFromMem(u32 start_index, u32* base, u32 num_registers);
  void WriteBoolRangeFromMem(u32 start_index, u32* base, u32 num_registers);
  void WriteLoopRangeFromMem(u32 start_index, u32* base, u32 num_registers);
  void WriteREGISTERSRangeFromMem(u32 start_index, u32* base, u32 num_registers);

  const xenos::reg::DC_LUT_30_COLOR* gamma_ramp_256_entry_table() const {
    return gamma_ramp_256_entry_table_;
  }
  const xenos::reg::DC_LUT_PWL_DATA* gamma_ramp_pwl_rgb() const { return gamma_ramp_pwl_rgb_[0]; }
  virtual void OnGammaRamp256EntryTableValueWritten() {}
  virtual void OnGammaRampPWLValueWritten() {}

  /**
   * Makes host memory coherent. COHER_STATUS_HOST often holds 0x01000000 or 0x03000000, and the
   * VC (vertex cache) or TC (texture cache) action it enables is logged. May run from the
   * WAIT_REG_MEM loop.
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
   * Executes a type 3 packet. Bit 0 is the predicate. When set, a bin check decides whether the
   * packet runs. Only type 3 packets are affected. Predicated swaps are skipped as they are never
   * valid.
   *
   * @param reader The ring buffer reader.
   * @param packet The packet header.
   * @return True when the packet executed.
   */
  bool ExecutePacketType3(memory::RingBuffer* reader, u32 packet);
  bool ExecutePacketType3_ME_INIT(memory::RingBuffer* reader, u32 packet, u32 count);
  bool ExecutePacketType3_NOP(memory::RingBuffer* reader, u32 packet, u32 count);
  bool ExecutePacketType3_INTERRUPT(memory::RingBuffer* reader, u32 packet, u32 count);
  /**
   * Executes the swap packet that VdSwap posts to request a screen swap and an interrupt. The
   * packet has 63 words and only the first has data.
   *
   * @param reader The ring buffer reader.
   * @param packet The packet header.
   * @param count The packet word count.
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
   * Executes the screen extent event. The callback tells the driver the xy coordinates affected by
   * the previous draw call, which is faked with the maximum extent.
   *
   * @param reader The ring buffer reader.
   * @param packet The packet header.
   * @param count The packet word count.
   * @return True when the packet executed.
   */
  bool ExecutePacketType3_EVENT_WRITE_EXT(memory::RingBuffer* reader, u32 packet, u32 count);
  /**
   * Executes the occlusion query event, sent on query begin and end. A fixed number of passed
   * samples is reported as a workaround.
   *
   * @param reader The ring buffer reader.
   * @param packet The packet header.
   * @param count The packet word count.
   * @return True when the packet executed.
   */
  virtual bool ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader, u32 packet,
                                                  u32 count);
  /**
   * Executes a draw. When viz_query_condition is not 0 the draw is conditional on a viz query. Bits
   * 0 to 5 hold the ID matching the one issued in PM4_VIZ_QUERY and bit 8 renders conditionally on
   * the query result. A draw the host cannot execute is dropped and the buffer keeps processing.
   *
   * @param reader The ring buffer reader.
   * @param packet The packet header.
   * @param opcode_name The opcode name for logging.
   * @param viz_query_condition The viz query token.
   * @param count_remaining The packet words left to read.
   * @return True when processing continues.
   */
  bool ExecutePacketType3Draw(memory::RingBuffer* reader, u32 packet, const char* opcode_name,
                              u32 viz_query_condition, u32 count_remaining);
  /**
   * Initiates a fetch of the index buffer and a draw. Generally used by Direct3D 9 for the kDMA and
   * kAutoIndex sources. The first token is a viz query token.
   *
   * @param reader The ring buffer reader.
   * @param packet The packet header.
   * @param count The packet word count.
   * @return True when processing continues.
   */
  bool ExecutePacketType3_DRAW_INDX(memory::RingBuffer* reader, u32 packet, u32 count);
  /**
   * Draws using the indices supplied in the packet. Generally used by Direct3D 9 for the kAutoIndex
   * source. There is no viz query token.
   *
   * @param reader The ring buffer reader.
   * @param packet The packet header.
   * @param count The packet word count.
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
   * the internal state of the scan converter on hardware, which differs from the register. The scan
   * converter writes its internal result back to the register at the end, which is faked as
   * visible in case it is read back.
   *
   * @param reader The ring buffer reader.
   * @param packet The packet header.
   * @param count The packet word count.
   * @return True when the packet executed.
   */
  bool ExecutePacketType3_VIZ_QUERY(memory::RingBuffer* reader, u32 packet, u32 count);

  virtual Shader* LoadShader(xenos::ShaderType shader_type, u32 guest_address,
                             const u32* host_address, u32 dword_count) = 0;

  virtual bool IssueDraw(xenos::PrimitiveType prim_type, u32 index_count,
                         IndexBufferInfo* index_buffer_info, bool major_mode_explicit) = 0;
  virtual bool IssueCopy() = 0;

  /**
   * Returns the swap post effect the command processor thread reads.
   *
   * @return The swap post effect.
   */
  SwapPostEffect GetActualSwapPostEffect() const { return swap_post_effect_actual_; }

  ReadbackResolveMode GetReadbackResolveMode(bool legacy_readback_resolve_enabled) const;
  bool IsReadbackMemexportEnabled(bool legacy_backend_flag) const;

  runtime::Memory* memory_ = nullptr;
  Runtime* runtime_ = nullptr;
  GraphicsSystem* graphics_system_ = nullptr;
  xenos::RegisterFile* register_file_ = nullptr;

  std::atomic<bool> worker_running_;
  X_HANDLE worker_thread_ = 0;

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

  // "Desired" is for the external thread managing the post-processing effect.
  SwapPostEffect swap_post_effect_desired_ = SwapPostEffect::kNone;
  SwapPostEffect swap_post_effect_actual_ = SwapPostEffect::kNone;

  /** Set by backend command processors to their legacy memexport readback cvar. */
  const rex::cvar::IVar* legacy_readback_memexport_cvar_ = nullptr;

 private:
  xenos::reg::DC_LUT_30_COLOR gamma_ramp_256_entry_table_[256] = {};
  xenos::reg::DC_LUT_PWL_DATA gamma_ramp_pwl_rgb_[128][3] = {};
  u32 gamma_ramp_rw_component_ = 0;
};

}  // namespace rex::gpu
