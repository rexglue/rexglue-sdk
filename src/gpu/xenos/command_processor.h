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
#include <functional>
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
#include "gpu/xenos/host/presenter.h"

namespace rex::gpu {

class GraphicsSystem;
class Shader;

enum class ReadbackResolveMode {
  kDisabled,
  kFast,
  kSome,
  kFull,
};

struct SwapState {
  // Lock must be held when changing data in this structure.
  std::mutex mutex;
  uint32_t width = 0;
  uint32_t height = 0;
  uintptr_t front_buffer_texture = 0;
  uintptr_t back_buffer_texture = 0;
  void* backend_data = nullptr;
  bool pending = false;
};

enum class SwapMode {
  kNormal,
  kIgnored,
};

enum class GammaRampType {
  kUnknown = 0,
  kTable,
  kPWL,
};

class CommandProcessor {
 public:
  enum class SwapPostEffect {
    kNone,
    kFxaa,
    kFxaaExtreme,
  };

  CommandProcessor(GraphicsSystem* graphics_system, Runtime& runtime);
  virtual ~CommandProcessor();

  uint32_t counter() const { return counter_; }
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

  void CallInThread(std::move_only_function<void()> fn);

  /**
   * Runs a function on the worker and waits for it to finish. The function runs on the calling
   * thread when that is the worker, or when the worker is not running.
   *
   * @param fn The function to run.
   */
  void CallInThreadAndWait(std::move_only_function<void()> fn);

  virtual void ClearCaches();
  virtual void InvalidateGpuMemory();

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
  virtual void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                         uint32_t frontbuffer_height) = 0;

  /**
   * Initializes the shader storage. May be called from a thread other than the command processor
   * thread while the command processor is paused, and its termination may be awaited.
   *
   * @param cache_root The cache root directory.
   * @param title_id The title ID.
   * @param blocking True to wait for the storage to finish loading.
   */
  virtual void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                                       bool blocking);

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2);
  /**
   * Enables writing the read pointer back to memory.
   *
   * @param ptr RB_RPTR_ADDR of CP_RB_RPTR_ADDR (0x70C), the address the read pointer is written to.
   * @param block_size_log2 RB_BLKSZ of CP_RB_CNTL (0x704), log2 of quadwords read between updates.
   */
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2);

  void UpdateWritePointer(uint32_t value);

  void ExecutePacket(uint32_t ptr, uint32_t count);

 protected:
  struct IndexBufferInfo {
    xenos::IndexFormat format = xenos::IndexFormat::kInt16;
    xenos::Endian endianness = xenos::Endian::kNone;
    uint32_t count = 0;
    uint32_t guest_base = 0;
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
  virtual void WriteRegister(uint32_t index, uint32_t value);
  uint32_t ReadRegisterValue(uint32_t index) const;
  virtual void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers);
  virtual void WriteRegisterRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                          uint32_t num_registers);
  void WriteALURangeFromRing(memory::RingBuffer* ring, uint32_t base, uint32_t num_registers);
  void WriteFetchRangeFromRing(memory::RingBuffer* ring, uint32_t base, uint32_t num_registers);
  void WriteBoolRangeFromRing(memory::RingBuffer* ring, uint32_t base, uint32_t num_registers);
  void WriteLoopRangeFromRing(memory::RingBuffer* ring, uint32_t base, uint32_t num_registers);
  void WriteREGISTERSRangeFromRing(memory::RingBuffer* ring, uint32_t base, uint32_t num_registers);
  void WriteALURangeFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers);
  void WriteFetchRangeFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers);
  void WriteBoolRangeFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers);
  void WriteLoopRangeFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers);
  void WriteREGISTERSRangeFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers);

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
  virtual void MakeCoherent();
  virtual void PrepareForWait();
  virtual void ReturnFromWait();

  uint32_t ExecutePrimaryBuffer(uint32_t start_index, uint32_t end_index);
  virtual void OnPrimaryBufferEnd() {}
  void ExecuteIndirectBuffer(uint32_t ptr, uint32_t length);
  bool ExecutePacket(memory::RingBuffer* reader);
  bool ExecutePacketType0(memory::RingBuffer* reader, uint32_t packet);
  bool ExecutePacketType1(memory::RingBuffer* reader, uint32_t packet);
  bool ExecutePacketType2(memory::RingBuffer* reader, uint32_t packet);
  /**
   * Executes a type 3 packet. Bit 0 is the predicate. When set, a bin check decides whether the
   * packet runs. Only type 3 packets are affected. Predicated swaps are skipped as they are never
   * valid.
   *
   * @param reader The ring buffer reader.
   * @param packet The packet header.
   * @return True when the packet executed.
   */
  bool ExecutePacketType3(memory::RingBuffer* reader, uint32_t packet);
  bool ExecutePacketType3_ME_INIT(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_NOP(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_INTERRUPT(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  /**
   * Executes the swap packet that VdSwap posts to request a screen swap and an interrupt. The
   * packet has 63 words and only the first has data.
   *
   * @param reader The ring buffer reader.
   * @param packet The packet header.
   * @param count The packet word count.
   * @return True when the packet executed.
   */
  bool ExecutePacketType3_XE_SWAP(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_INDIRECT_BUFFER(memory::RingBuffer* reader, uint32_t packet,
                                          uint32_t count);
  bool ExecutePacketType3_WAIT_REG_MEM(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_REG_RMW(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_REG_TO_MEM(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_MEM_WRITE(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_COND_WRITE(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_EVENT_WRITE(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_EVENT_WRITE_SHD(memory::RingBuffer* reader, uint32_t packet,
                                          uint32_t count);
  /**
   * Executes the screen extent event. The callback tells the driver the xy coordinates affected by
   * the previous draw call, which is faked with the maximum extent.
   *
   * @param reader The ring buffer reader.
   * @param packet The packet header.
   * @param count The packet word count.
   * @return True when the packet executed.
   */
  bool ExecutePacketType3_EVENT_WRITE_EXT(memory::RingBuffer* reader, uint32_t packet,
                                          uint32_t count);
  /**
   * Executes the occlusion query event, sent on query begin and end. A fixed number of passed
   * samples is reported as a workaround.
   *
   * @param reader The ring buffer reader.
   * @param packet The packet header.
   * @param count The packet word count.
   * @return True when the packet executed.
   */
  virtual bool ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count);
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
  bool ExecutePacketType3Draw(memory::RingBuffer* reader, uint32_t packet, const char* opcode_name,
                              uint32_t viz_query_condition, uint32_t count_remaining);
  /**
   * Initiates a fetch of the index buffer and a draw. Generally used by Direct3D 9 for the kDMA and
   * kAutoIndex sources. The first token is a viz query token.
   *
   * @param reader The ring buffer reader.
   * @param packet The packet header.
   * @param count The packet word count.
   * @return True when processing continues.
   */
  bool ExecutePacketType3_DRAW_INDX(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  /**
   * Draws using the indices supplied in the packet. Generally used by Direct3D 9 for the kAutoIndex
   * source. There is no viz query token.
   *
   * @param reader The ring buffer reader.
   * @param packet The packet header.
   * @param count The packet word count.
   * @return True when processing continues.
   */
  bool ExecutePacketType3_DRAW_INDX_2(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_SET_CONSTANT(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_SET_CONSTANT2(memory::RingBuffer* reader, uint32_t packet,
                                        uint32_t count);
  bool ExecutePacketType3_LOAD_ALU_CONSTANT(memory::RingBuffer* reader, uint32_t packet,
                                            uint32_t count);
  bool ExecutePacketType3_SET_SHADER_CONSTANTS(memory::RingBuffer* reader, uint32_t packet,
                                               uint32_t count);
  bool ExecutePacketType3_IM_LOAD(memory::RingBuffer* reader, uint32_t packet, uint32_t count);
  bool ExecutePacketType3_IM_LOAD_IMMEDIATE(memory::RingBuffer* reader,

                                            uint32_t packet, uint32_t count);
  bool ExecutePacketType3_INVALIDATE_STATE(memory::RingBuffer* reader, uint32_t packet,
                                           uint32_t count);
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
  bool ExecutePacketType3_VIZ_QUERY(memory::RingBuffer* reader, uint32_t packet, uint32_t count);

  virtual Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                             const uint32_t* host_address, uint32_t dword_count) = 0;

  virtual bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
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
  std::queue<std::move_only_function<void()>> pending_fns_;

  std::vector<uint32_t> me_bin_;

  uint32_t counter_ = 0;

  uint32_t primary_buffer_ptr_ = 0;
  uint32_t primary_buffer_size_ = 0;

  uint32_t read_ptr_index_ = 0;
  uint32_t read_ptr_update_freq_ = 0;
  uint32_t read_ptr_writeback_ptr_ = 0;

  std::unique_ptr<rex::platform::Event> write_ptr_index_event_;
  std::atomic<uint32_t> write_ptr_index_;

  /** Register writes beyond the emulated register file range, kept for dependent packet logic. */
  std::unordered_map<uint32_t, uint32_t> extended_register_values_;

  uint64_t bin_select_ = 0xFFFFFFFFull;
  uint64_t bin_mask_ = 0xFFFFFFFFull;

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
  uint32_t gamma_ramp_rw_component_ = 0;
};

}  // namespace rex::gpu
