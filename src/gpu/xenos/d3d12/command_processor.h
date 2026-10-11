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

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include "gpu/xenos/command_processor.h"
#include "gpu/xenos/d3d12/deferred_command_list.h"
#include "gpu/xenos/d3d12/graphics_system.h"
#include "gpu/xenos/d3d12/pipeline_cache.h"
#include "gpu/xenos/d3d12/primitive_processor.h"
#include "gpu/xenos/d3d12/render_target_cache.h"
#include "gpu/xenos/d3d12/shared_memory.h"
#include "gpu/xenos/d3d12/texture_cache.h"
#include "gpu/xenos/pipeline/shader/dxbc.h"
#include "gpu/xenos/pipeline/shader/dxbc_translator.h"
#include <rex/xenos/registers.h>
#include "gpu/xenos/draw_state.h"
#include <rex/xenos/xenos.h>
#include <rex/runtime.h>
#include "gpu/xenos/host/d3d12/d3d12_descriptor_heap_pool.h"
#include "gpu/xenos/host/d3d12/d3d12_provider.h"
#include "gpu/xenos/host/d3d12/d3d12_upload_buffer_pool.h"
#include "gpu/xenos/host/d3d12/d3d12_objects.h"

namespace rex::gpu::d3d12 {

class D3D12CommandProcessor : public CommandProcessor {
 public:
  explicit D3D12CommandProcessor(D3D12GraphicsSystem* graphics_system, Runtime& runtime);
  ~D3D12CommandProcessor();

  void ClearCaches() override;
  void InvalidateGPUMemory() override;

  void InitializeShaderStorage(const std::filesystem::path& cache_root, u32 title_id,
                               bool blocking) override;

  gpu::d3d12::D3D12Provider& GetD3D12Provider() const {
    return *static_cast<gpu::d3d12::D3D12Provider*>(graphics_system_->provider());
  }

  /**
   * Returns the deferred drawing command list for the open submission.
   *
   * @return The deferred command list.
   */
  DeferredCommandList& GetDeferredCommandList() {
    REX_ASSERT(submission_open_);
    return deferred_command_list_;
  }

  u64 GetCurrentSubmission() const { return submission_current_; }
  u64 GetCompletedSubmission() const { return submission_completed_; }

  /**
   * Tracks queue operations done outside submissions, such as UpdateTileMappings, so
   * CheckSubmissionFence(submission_current_) can await them when they follow the latest
   * ExecuteCommandLists and Signal.
   */
  void NotifyQueueOperationsDoneDirectly() {
    queue_operations_done_since_submission_signal_ = true;
  }

  u64 GetCurrentFrame() const { return frame_current_; }
  u64 GetCompletedFrame() const { return frame_completed_; }

  /**
   * Pushes a transition barrier.
   *
   * @param resource Resource to transition.
   * @param old_state State before the transition.
   * @param new_state State after the transition.
   * @param subresource Subresource index, or all subresources.
   * @return True when the barrier was inserted, as the new state differs.
   */
  bool PushTransitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES old_state,
                             D3D12_RESOURCE_STATES new_state,
                             UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
  void PushAliasingBarrier(ID3D12Resource* old_resource, ID3D12Resource* new_resource);
  void PushUAVBarrier(ID3D12Resource* resource);
  void SubmitBarriers();

  /**
   * Finds or creates the root signature for a pipeline.
   *
   * @param vertex_shader Vertex shader of the pipeline.
   * @param pixel_shader Pixel shader of the pipeline, may be nullptr.
   * @param tessellated True when the pipeline is tessellated.
   * @return The root signature.
   */
  ID3D12RootSignature* GetRootSignature(const DXBCShader* vertex_shader,
                                        const DXBCShader* pixel_shader, bool tessellated);

  gpu::d3d12::D3D12UploadBufferPool& GetConstantBufferPool() const {
    return *constant_buffer_pool_;
  }

  D3D12_CPU_DESCRIPTOR_HANDLE GetViewBindlessHeapCPUStart() const {
    REX_ASSERT(bindless_resources_used_);
    return view_bindless_heap_cpu_start_;
  }
  /**
   * Requests a persistent bindless view descriptor. If the unbounded SRV range for bindless
   * resources is used in the root signature of the draw or dispatch referencing the
   * descriptor, it must only allocate SRVs, otherwise root signature creation fails on Nvidia
   * Fermi.
   *
   * @return The descriptor index, or UINT32_MAX if no descriptors are free.
   */
  u32 RequestPersistentViewBindlessDescriptor();
  void ReleaseViewBindlessDescriptorImmediately(u32 descriptor_index) noexcept;
  /**
   * Requests non contiguous CBV, SRV and UAV descriptors for use only within the next internal
   * draw or dispatch. May change the current descriptor heap. If the unbounded SRV range for
   * bindless resources is used in the root signature of the draw or dispatch referencing
   * the descriptors, it must only allocate SRVs, otherwise root signature creation fails on
   * Nvidia Fermi.
   *
   * @param count Number of descriptors.
   * @param handles_out Receives count handle pairs.
   * @return True when the descriptors were allocated.
   */
  bool RequestOneUseSingleViewDescriptors(
      u32 count, gpu::d3d12::objects::DescriptorCPUGPUHandlePair* handles_out);
  /** Needed often, so always allocated. */
  enum class SystemBindlessView : u8 {
    /** Both may be bound as one root parameter. */
    kSharedMemoryRawSRVAndNullRawUAVStart,
    kSharedMemoryRawSRV = kSharedMemoryRawSRVAndNullRawUAVStart,
    kNullRawUAV,

    /** Both may be bound as one root parameter. */
    kNullRawSRVAndSharedMemoryRawUAVStart,
    kNullRawSRV = kNullRawSRVAndSharedMemoryRawUAVStart,
    kSharedMemoryRawUAV,

    kSharedMemoryR32UintSRV,
    kSharedMemoryR32G32UintSRV,
    kSharedMemoryR32G32B32A32UintSRV,
    kSharedMemoryR32UintUAV,
    kSharedMemoryR32G32UintUAV,
    kSharedMemoryR32G32B32A32UintUAV,

    kEDRAMRawSRV,
    kEDRAMR32UintSRV,
    kEDRAMR32G32UintSRV,
    kEDRAMR32G32B32A32UintSRV,
    kEDRAMRawUAV,
    kEDRAMR32UintUAV,
    kEDRAMR32G32UintUAV,
    kEDRAMR32G32B32A32UintUAV,

    kGammaRampTableSRV,
    kGammaRampPWLSRV,

    /**
     * SRVs from here are accessible to shaders through an unbounded range, so no descriptors of
     * other types bound alongside unbounded ranges may be located from here.
     */
    kUnboundedSRVsStart,
    kNullTexture2DArray = kUnboundedSRVsStart,
    kNullTexture3D,
    kNullTextureCube,

    kCount,
  };
  gpu::d3d12::objects::DescriptorCPUGPUHandlePair GetSystemBindlessViewHandlePair(
      SystemBindlessView view) const;
  gpu::d3d12::objects::DescriptorCPUGPUHandlePair GetSharedMemoryUintPow2BindlessSRVHandlePair(
      u32 element_size_bytes_pow2) const;
  gpu::d3d12::objects::DescriptorCPUGPUHandlePair GetSharedMemoryUintPow2BindlessUAVHandlePair(
      u32 element_size_bytes_pow2) const;
  gpu::d3d12::objects::DescriptorCPUGPUHandlePair GetEDRAMUintPow2BindlessSRVHandlePair(
      u32 element_size_bytes_pow2) const;
  gpu::d3d12::objects::DescriptorCPUGPUHandlePair GetEDRAMUintPow2BindlessUAVHandlePair(
      u32 element_size_bytes_pow2) const;

  /**
   * Returns a single temporary GPU side buffer within a submission for tasks like texture
   * untiling and resolving.
   *
   * @param size Size in bytes.
   * @param state State the buffer is in on return.
   * @return The buffer.
   */
  ID3D12Resource* RequestScratchGPUBuffer(u32 size, D3D12_RESOURCE_STATES state);
  /**
   * Releases the scratch buffer and stores its new state in case the user transitioned it.
   *
   * @param buffer Buffer from RequestScratchGPUBuffer.
   * @param new_state State of the buffer at release.
   */
  void ReleaseScratchGPUBuffer(ID3D12Resource* buffer, D3D12_RESOURCE_STATES new_state);

  /**
   * Returns a pipeline with deferred creation by its handle, or nullptr if creation failed.
   *
   * @param handle Pipeline handle.
   * @return The pipeline state, or nullptr.
   */
  ID3D12PipelineState* GetD3D12PipelineByHandle(void* handle) const {
    return pipeline_cache_->GetD3D12PipelineByHandle(handle);
  }

  /**
   * Sets the current cached values to external ones, mainly for cache invalidation. A submission
   * must be open.
   *
   * @param pipeline External pipeline.
   */
  void SetExternalPipeline(ID3D12PipelineState* pipeline);
  void SetExternalGraphicsRootSignature(ID3D12RootSignature* root_signature);
  void SetViewport(const D3D12_VIEWPORT& viewport);
  void SetScissorRect(const D3D12_RECT& scissor_rect);
  void SetStencilReference(u32 stencil_ref);
  void SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY primitive_topology);

  /**
   * Returns the text for the GPU backend name in the window title.
   *
   * @return The text.
   */
  std::string GetWindowTitleText() const;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  void WriteRegister(u32 index, u32 value) override;
  void WriteRegistersFromMem(u32 start_index, u32* base, u32 num_registers) override;
  bool ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader, u32 packet,
                                          u32 count) override;

  void OnGammaRamp256EntryTableValueWritten() override;
  void OnGammaRampPWLValueWritten() override;

  void IssueSwap(u32 frontbuffer_ptr, u32 frontbuffer_width, u32 frontbuffer_height) override;

  void OnPrimaryBufferEnd() override;

  Shader* LoadShader(xenos::ShaderType shader_type, u32 guest_address, const u32* host_address,
                     u32 dword_count) override;

  bool IssueDraw(xenos::PrimitiveType primitive_type, u32 index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  static constexpr u32 kQueueFrames = 3;

  /**
   * Root parameters. The root signature at each stage must stay at 13 dwords or less (better 12
   * or less) to fit in user data on AMD. Descriptor tables take 1 dword and root descriptors take
   * 2 dwords, though root descriptors need less CPU setup, so the balance must be kept. CBVs are
   * set through root descriptors in both bindful and bindless cases. Bindful resources use
   * multiple root signatures depending on extra parameters. Bindless resources use 2 global root
   * signatures, for non tessellated and tessellated drawing.
   */
  enum RootParameter : u8 {
    /** Changes often, especially for UI draws and models drawn in parts. Holds fetch constants. */
    kRootParameter_Bindful_FetchConstants = 0, /**< +2 dwords = 2 in all. */
    /** Changes often, such as for an object drawn repeatedly. May hold projection matrices. */
    kRootParameter_Bindful_FloatConstantsVertex, /**< +2 = 4 in VS. */
    /** Changes less often, per material. */
    kRootParameter_Bindful_FloatConstantsPixel, /**< +2 = 4 in PS. */
    /** May stay the same across many draws. */
    kRootParameter_Bindful_SystemConstants, /**< +2 = 6 in all. */
    /** Rarely used and changed, holds flow control constants. */
    kRootParameter_Bindful_BoolLoopConstants, /**< +2 = 8 in all. */
    /**
     * Changes only when starting a new descriptor heap or switching shared memory between SRV
     * and UAV. Holds the shared memory byte address buffer (SRV and UAV, either may be null) and,
     * if ROV is used for EDRAM, the EDRAM R32_UINT UAV.
     */
    kRootParameter_Bindful_SharedMemoryAndEDRAM, /**< +1 = 9 in all. */

    kRootParameter_Bindful_Count_Base,

    /**
     * Extra parameters that may exist are pixel textures (+1 = 10 in PS), pixel samplers (+1 = 11
     * in PS), vertex textures (+1 = 10 in VS) and vertex samplers (+1 = 11 in VS).
     */
    kRootParameter_Bindful_Count_Max = kRootParameter_Bindful_Count_Base + 4,

    kRootParameter_Bindless_FetchConstants = 0,   /**< +2 = 2 in all. */
    kRootParameter_Bindless_FloatConstantsVertex, /**< +2 = 4 in VS. */
    kRootParameter_Bindless_FloatConstantsPixel,  /**< +2 = 4 in PS. */
    /** Texture and sampler descriptor indices, changed per material. */
    kRootParameter_Bindless_DescriptorIndicesPixel,  /**< +2 = 6 in PS. */
    kRootParameter_Bindless_DescriptorIndicesVertex, /**< +2 = 6 in VS. */
    kRootParameter_Bindless_SystemConstants,         /**< +2 = 8 in all. */
    kRootParameter_Bindless_BoolLoopConstants,       /**< +2 = 10 in all. */
    /**
     * Changes only when switching shared memory between SRV and UAV. Holds the shared memory
     * byte address buffer (SRV and UAV, either may be null).
     */
    kRootParameter_Bindless_SharedMemory, /**< +1 = 11 in all. */
    /** Unbounded sampler descriptor table, changed in case of overflow. */
    kRootParameter_Bindless_SamplerHeap, /**< +1 = 12 in all. */
    /** Unbounded SRV and UAV descriptor table, never changed. */
    kRootParameter_Bindless_ViewHeap, /**< +1 = 13 in all. */

    kRootParameter_Bindless_Count,
  };

  struct RootBindfulExtraParameterIndices {
    u32 textures_pixel;
    u32 samplers_pixel;
    u32 textures_vertex;
    u32 samplers_vertex;
    static constexpr u32 kUnavailable = UINT32_MAX;
  };
  /**
   * Returns the indices of optional root parameters.
   *
   * @param vertex_shader Vertex shader of the pipeline.
   * @param pixel_shader Pixel shader of the pipeline, may be nullptr.
   * @param indices_out Receives the indices.
   * @return The total parameter count.
   */
  static u32 GetRootBindfulExtraParameterIndices(const DXBCShader* vertex_shader,
                                                 const DXBCShader* pixel_shader,
                                                 RootBindfulExtraParameterIndices& indices_out);

  /**
   * Rechecks the submission number and reclaims per submission resources.
   *
   * @param await_submission Submission to await, 0 to only check the status, or
   * submission_current_ to wait for all queue operations to complete.
   */
  void CheckSubmissionFence(u64 await_submission);
  /**
   * Begins a submission. May be called at any time. If a non frame submission is open, passing
   * true promotes it to a frame. Submission (ExecuteCommandLists) boundaries are implicit full
   * UAV and aliasing barriers and cause common resource state promotion and decay.
   *
   * @param is_guest_command True to open a new full frame if pending, with full resource cleanup
   * and, if needed, starting capture, rather than resuming after mid frame synchronization.
   *
   * @return True when a submission is open and the device is not removed.
   */
  bool BeginSubmission(bool is_guest_command);
  /**
   * Ends a submission. May be called at any time. Passing true closes the frame regardless of
   * whether the submission is closed, with cache clearing and stopping capture if needed.
   * On failure the submission stays open.
   *
   * @param is_swap True to close a full frame.
   * @return True when the submission was done successfully.
   */
  bool EndSubmission(bool is_swap);
  /**
   * Reports whether ending a submission now would not cost more delay than it saves by starting
   * the GPU earlier, such as with unfinished graphics pipeline creation requests that must be
   * fulfilled before submitting the command list.
   *
   * @return True when the submission can end immediately.
   */
  bool CanEndSubmissionImmediately() const;
  bool AwaitAllQueueOperationsCompletion() {
    CheckSubmissionFence(submission_current_);
    return submission_completed_ + 1 >= submission_current_;
  }
  void LogDeviceRemovalDiagnostics(ID3D12Device* device, HRESULT reason);

  void UpdateDebugMarkersEnabled();
  void PushDebugMarker(const char* format, ...);
  void PopDebugMarker();
  void InsertDebugMarker(const char* format, ...);
  bool debug_markers_enabled() const { return debug_markers_enabled_; }

  // Submission completion must be awaited before this.
  void ClearCommandAllocatorCache();

  /**
   * Requests descriptors and rebinds the descriptor heap on the draw command list. See
   * D3D12DescriptorHeapPool::Request for the partial and full update explanation. Does not work
   * with bindless descriptors.
   *
   * @param previous_heap_index Heap index from the previous request.
   * @param count_for_partial_update Descriptor count for a partial update.
   * @param count_for_full_update Descriptor count for a full update.
   * @param cpu_handle_out Receives the CPU handle.
   * @param gpu_handle_out Receives the GPU handle.
   * @return The heap index.
   */
  u64 RequestViewBindfulDescriptors(u64 previous_heap_index, u32 count_for_partial_update,
                                    u32 count_for_full_update,
                                    D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out,
                                    D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out);
  u64 RequestSamplerBindfulDescriptors(u64 previous_heap_index, u32 count_for_partial_update,
                                       u32 count_for_full_update,
                                       D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out,
                                       D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out);

  void UpdateFixedFunctionState(const draw_state::ViewportInfo& viewport_info,
                                const draw_state::Scissor& scissor, bool primitive_polygonal,
                                xenos::reg::RB_DEPTHCONTROL normalized_depth_control);
  void UpdateSystemConstantValues(bool shared_memory_is_uav, bool primitive_polygonal,
                                  u32 line_loop_closing_index, xenos::Endian index_endian,
                                  const draw_state::ViewportInfo& viewport_info,
                                  u32 used_texture_mask,
                                  xenos::reg::RB_DEPTHCONTROL normalized_depth_control,
                                  u32 normalized_color_mask);
  bool UpdateBindings(const D3D12Shader* vertex_shader, const D3D12Shader* pixel_shader,
                      ID3D12RootSignature* root_signature, bool shared_memory_is_uav);
  bool IssueCopy_ReadbackResolvePath();
  bool IssueDraw_MemexportReadbackFullPath(u32 total_size);
  bool IssueDraw_MemexportReadbackFastPath(u32 total_size);

  /**
   * Returns a buffer for reading GPU data back to the CPU, assuming synchronization right after
   * use. Always in COPY_DEST state.
   *
   * @param size Size in bytes.
   * @return The buffer.
   */
  ID3D12Resource* RequestReadbackBuffer(u32 size);
  struct ReadbackBuffer {
    ID3D12Resource* buffers[2] = {nullptr, nullptr};
    u32 sizes[2] = {0, 0};
    void* mapped_data[2] = {nullptr, nullptr};
    u64 submission_written[2] = {0, 0};
    u32 written_size[2] = {0, 0};
    u32 current_index = 0;
    u64 last_used_frame = 0;
  };
  void EvictOldReadbackBuffers(std::unordered_map<u64, ReadbackBuffer>& buffer_map) const;
  static constexpr u32 kReadbackBufferSizeIncrement = 16 * 1024 * 1024;
  static constexpr size_t kMaxReadbackBuffers = 256;
  static constexpr u64 kReadbackBufferEvictionAgeFrames = 60;
  static u32 AlignReadbackBufferSize(u32 size) {
    if (size < 1 * 1024 * 1024) {
      return rex::align(size, 256u * 1024u);
    }
    if (size < 4 * 1024 * 1024) {
      return rex::align(size, 1u * 1024u * 1024u);
    }
    return rex::align(size, kReadbackBufferSizeIncrement);
  }
  static u64 BuildReadbackResolveKey(u32 address, u32 length) {
    return (static_cast<u64>(address) << 32) | static_cast<u64>(length);
  }
  static u64 BuildMemexportReadbackKey(u32 first_base_address_dwords, u32 total_size) {
    return (static_cast<u64>(first_base_address_dwords) << 32) | static_cast<u64>(total_size);
  }

  bool InitializeOcclusionQueryResources();
  void ShutdownOcclusionQueryResources();
  bool BeginGuestOcclusionQuery(u32 sample_count_address);
  bool EndGuestOcclusionQuery(xenos::DepthSampleCounts* sample_counts);
  bool AcquireOcclusionQueryIndex(u32& host_index_out);
  void DisableHostOcclusionQueries();
  u64 NormalizeOcclusionSamples(u64 samples) const;
  void WriteGuestOcclusionResult(xenos::DepthSampleCounts* sample_counts, u64 samples);
  void InvalidateAllVertexBufferResidency();
  void InvalidateVertexBufferResidency(u32 vfetch_index);
  void InvalidateVertexBufferResidencyRange(u32 first_vfetch, u32 last_vfetch);

  void WriteGammaRampSRV(bool is_pwl, D3D12_CPU_DESCRIPTOR_HANDLE handle) const;

  bool device_removed_ = false;

  bool cache_clear_requested_ = false;

  HANDLE fence_completion_event_ = nullptr;

  bool submission_open_ = false;
  /** Values of submission_fence_. */
  u64 submission_current_ = 1;
  u64 submission_completed_ = 0;
  ID3D12Fence* submission_fence_ = nullptr;

  /**
   * Awaits non submission queue operations such as UpdateTileMappings in
   * AwaitAllQueueOperationsCompletion when they are queued after the latest ExecuteCommandLists
   * and Signal, as awaiting the submission does not cover them.
   */
  ID3D12Fence* queue_operations_since_submission_fence_ = nullptr;
  u64 queue_operations_since_submission_fence_last_ = 0;
  bool queue_operations_done_since_submission_signal_ = false;

  bool frame_open_ = false;
  /** Title frame index, updated at the start of a frame, as resources span submissions. */
  u64 frame_current_ = 1;
  u64 frame_completed_ = 0;
  u64 closed_frame_submissions_[kQueueFrames] = {};

  struct CommandAllocator {
    ID3D12CommandAllocator* command_allocator;
    u64 last_usage_submission;
    CommandAllocator* next;
  };
  CommandAllocator* command_allocator_writable_first_ = nullptr;
  CommandAllocator* command_allocator_writable_last_ = nullptr;
  CommandAllocator* command_allocator_submitted_first_ = nullptr;
  CommandAllocator* command_allocator_submitted_last_ = nullptr;
  ID3D12GraphicsCommandList* command_list_ = nullptr;
  ID3D12GraphicsCommandList1* command_list_1_ = nullptr;
  DeferredCommandList deferred_command_list_;

  bool debug_markers_enabled_ = false;

  /** Caches viewport info to skip GetHostViewportInfo when affecting registers are unchanged. */
  struct ViewportCacheKey {
    u32 pa_cl_clip_cntl;
    u32 pa_cl_vte_cntl;
    u32 pa_su_sc_mode_cntl;
    u32 pa_su_vtx_cntl;
    u32 pa_sc_window_offset;
    u32 normalized_depth_control;
    /** Register order is XSCALE, XOFFSET, YSCALE, YOFFSET, ZSCALE, ZOFFSET. */
    u32 vport_regs[6];
    /** Packed convert_z_to_float24, full_float24 and ps_writes_depth. */
    u32 flags;
    bool operator==(const ViewportCacheKey&) const = default;
  };
  ViewportCacheKey previous_viewport_key_{};
  draw_state::ViewportInfo previous_viewport_info_{};
  bool viewport_cache_valid_ = false;

  /**
   * True to use bindless textures and samplers, which make UpdateBindings many times faster than
   * bindful, where CopyDescriptorsSimple takes most of the time.
   */
  bool bindless_resources_used_ = false;

  std::unique_ptr<D3D12SharedMemory> shared_memory_;

  std::unique_ptr<D3D12RenderTargetCache> render_target_cache_;

  static constexpr u32 kTitleStateValid = u32{1} << 31;
  std::atomic<u32> title_state_{0};

  std::unique_ptr<gpu::d3d12::D3D12UploadBufferPool> constant_buffer_pool_;

  static constexpr u32 kViewBindfulHeapSize = 32768;
  static_assert(kViewBindfulHeapSize <= D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1);
  std::unique_ptr<gpu::d3d12::D3D12DescriptorHeapPool> view_bindful_heap_pool_;
  /** Bound descriptor heap, updated by RequestViewBindfulDescriptors. */
  ID3D12DescriptorHeap* view_bindful_heap_current_;
  /**
   * Textures have 4 KB alignment in guest memory, so at most 512 MB / 4 KB exist, times 3 for
   * swizzles, signedness, multiple host textures per guest texture and transient descriptors.
   * In practice far fewer exist, so this is a safe value. The limit is 1000000 for resource
   * binding tier 2.
   */
  static constexpr u32 kViewBindlessHeapSize = 262144;
  static_assert(kViewBindlessHeapSize <= D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_2);
  ID3D12DescriptorHeap* view_bindless_heap_ = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE view_bindless_heap_cpu_start_;
  D3D12_GPU_DESCRIPTOR_HANDLE view_bindless_heap_gpu_start_;
  u32 view_bindless_heap_allocated_ = 0;
  std::vector<u32> view_bindless_heap_free_;
  u32 view_bindless_heap_free_count_ = 0;
  /** Pairs of descriptor index and requesting submission, sorted by submission number. */
  std::deque<std::pair<u32, u64>> view_bindless_one_use_descriptors_;

  /**
   * Direct3D 12 allows shader visible heaps of at most 2048 samplers (due to Nvidia addressing).
   * A possible Nvidia driver bug (440.97 and earlier on Windows 10 1803) broke sampler 2047 when
   * a heap of 8 or fewer samplers also exists, such as the immediate drawer sampler heap.
   */
  static constexpr u32 kSamplerHeapSize = 2000;
  static_assert(kSamplerHeapSize <= D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE);
  std::unique_ptr<gpu::d3d12::D3D12DescriptorHeapPool> sampler_bindful_heap_pool_;
  ID3D12DescriptorHeap* sampler_bindful_heap_current_;
  ID3D12DescriptorHeap* sampler_bindless_heap_current_ = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE sampler_bindless_heap_cpu_start_;
  D3D12_GPU_DESCRIPTOR_HANDLE sampler_bindless_heap_gpu_start_;
  /**
   * Only texture cache samplers use the heap, so samplers are never freed and a linear allocator
   * without a free list is used.
   */
  u32 sampler_bindless_heap_allocated_ = 0;
  /**
   * Pairs of heap and overflow submission number, kept when the total sampler count exceeds
   * kSamplerHeapSize and the heap is switched. That can happen as Direct3D 9 has sampler
   * parameter state, so 1 changed parameter may double the sampler count. Sorted by submission
   * number, so checking the first for reuse is enough.
   */
  std::deque<std::pair<ID3D12DescriptorHeap*, u64>> sampler_bindless_heaps_overflowed_;
  /** Maps D3D12TextureCache::SamplerParameters::value to indices in the current bindless heap. */
  std::unordered_map<u32, u32> texture_cache_bindless_sampler_map_;

  std::unordered_map<u32, ID3D12RootSignature*> root_signatures_bindful_;
  ID3D12RootSignature* root_signature_bindless_vs_ = nullptr;
  ID3D12RootSignature* root_signature_bindless_ds_ = nullptr;

  std::unique_ptr<D3D12PrimitiveProcessor> primitive_processor_;

  std::unique_ptr<PipelineCache> pipeline_cache_;

  std::unique_ptr<D3D12TextureCache> texture_cache_;

  /**
   * Bytes 0x0 to 0x3FF hold the 256 entry gamma ramp table as B10G10R10X2, read as R10G10B10X2
   * with swizzle. Bytes 0x400 to 0x9FF hold the 128 entry PWL R16G16 gamma ramp with R as base and
   * G as delta, the low 6 bits of each zero, and 3 elements per entry.
   */
  Microsoft::WRL::ComPtr<ID3D12Resource> gamma_ramp_buffer_;
  D3D12_RESOURCE_STATES gamma_ramp_buffer_state_;
  /** Upload buffer for an image like gamma_ramp_buffer_ with kQueueFrames array layers. */
  Microsoft::WRL::ComPtr<ID3D12Resource> gamma_ramp_upload_buffer_;
  u8* gamma_ramp_upload_buffer_mapping_ = nullptr;
  bool gamma_ramp_256_entry_table_up_to_date_ = false;
  bool gamma_ramp_pwl_up_to_date_ = false;

  struct ApplyGammaConstants {
    u32 size[2];
  };
  enum class ApplyGammaRootParameter : u8 {
    kConstants,
    kDestination,
    kSource,
    kRamp,

    kCount,
  };
  Microsoft::WRL::ComPtr<ID3D12RootSignature> apply_gamma_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> apply_gamma_table_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> apply_gamma_table_fxaa_luma_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> apply_gamma_pwl_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> apply_gamma_pwl_fxaa_luma_pipeline_;

  struct FxaaConstants {
    u32 size[2];
    f32 size_inv[2];
  };
  enum class FxaaRootParameter : u8 {
    kConstants,
    kDestination,
    kSource,

    kCount,
  };
  Microsoft::WRL::ComPtr<ID3D12RootSignature> fxaa_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> fxaa_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> fxaa_extreme_pipeline_;

  struct ResolveDownscaleConstants {
    u32 scale_x;
    u32 scale_y;
    u32 pixel_size_log2;
    u32 tile_count;
    u32 half_pixel_offset;
  };
  enum class ResolveDownscaleRootParameter : u8 {
    kConstants,
    kSource,
    kDestination,

    kCount,
  };
  Microsoft::WRL::ComPtr<ID3D12RootSignature> resolve_downscale_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> resolve_downscale_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12Resource> resolve_downscale_buffer_;
  u32 resolve_downscale_buffer_size_ = 0;

  /**
   * The PWL gamma ramp can produce more than 10 bits per channel, which has no visible effect.
   * The original FXAA shader needs luma in the alpha channel, so R16G16B16A16_UNORM keeps all 13
   * bits generated with an increment of 2^3 and leaves room for fractional luma weights. That
   * avoids modifying the FXAA shader or adding fetches for a separate alpha texture.
   */
  static constexpr DXGI_FORMAT kFxaaSourceTextureFormat = DXGI_FORMAT_R16G16B16A16_UNORM;
  /** Kept in NON_PIXEL_SHADER_RESOURCE state. */
  Microsoft::WRL::ComPtr<ID3D12Resource> fxaa_source_texture_;
  u64 fxaa_source_texture_submission_ = 0;

  std::vector<D3D12_RESOURCE_BARRIER> barriers_;

  /** Pairs of requesting submission and resource, sorted by submission number. */
  std::deque<std::pair<u64, ID3D12Resource*>> resources_for_deletion_;

  static constexpr u32 kScratchBufferSizeIncrement = 16 * 1024 * 1024;
  ID3D12Resource* scratch_buffer_ = nullptr;
  u32 scratch_buffer_size_ = 0;
  D3D12_RESOURCE_STATES scratch_buffer_state_;
  bool scratch_buffer_used_ = false;

  ID3D12Resource* readback_buffer_ = nullptr;
  u32 readback_buffer_size_ = 0;
  std::unordered_map<u64, ReadbackBuffer> readback_buffers_;
  std::unordered_map<u64, ReadbackBuffer> memexport_readback_buffers_;

  static constexpr u32 kMaxOcclusionQueries = 8192;
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> occlusion_query_heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> occlusion_query_readback_;
  u64* occlusion_query_readback_mapping_ = nullptr;
  u32 occlusion_query_cursor_ = 0;
  bool occlusion_query_resources_available_ = false;
  struct ActiveOcclusionQuery {
    u32 sample_count_address = 0;
    u32 host_index = UINT32_MAX;
    bool valid = false;
  } active_occlusion_query_;
  struct VertexBufferState {
    u32 address = UINT32_MAX;
    u32 size = UINT32_MAX;
  };
  std::array<VertexBufferState, 96> vertex_buffer_states_{};
  u64 vertex_buffers_in_sync_[2] = {};

  D3D12_VIEWPORT ff_viewport_;
  D3D12_RECT ff_scissor_;
  f32 ff_blend_factor_[4];
  u32 ff_stencil_ref_;
  bool ff_viewport_update_needed_;
  bool ff_scissor_update_needed_;
  bool ff_blend_factor_update_needed_;
  bool ff_stencil_ref_update_needed_;

  /**
   * Bound pipeline, either a graphics pipeline from the pipeline cache, possibly with deferred
   * creation, where current_external_pipeline_ is nullptr, or a non Xenos graphics or compute
   * pipeline, where current_guest_pipeline_ is nullptr.
   */
  void* current_guest_pipeline_;
  ID3D12PipelineState* current_external_pipeline_;

  ID3D12RootSignature* current_graphics_root_signature_;
  RootBindfulExtraParameterIndices current_graphics_root_bindful_extras_;
  /** Bits of the root parameters that are up to date, reset when a new signature is bound. */
  u32 current_graphics_root_up_to_date_;

  DXBCShaderTranslator::SystemConstants system_constants_;

  /** Float constant usage masks of the last draw call. */
  u64 current_float_constant_map_vertex_[4];
  u64 current_float_constant_map_pixel_[4];

  struct ConstantBufferBinding {
    D3D12_GPU_VIRTUAL_ADDRESS address;
    bool up_to_date;
  };
  ConstantBufferBinding cbuffer_binding_system_;
  ConstantBufferBinding cbuffer_binding_float_vertex_;
  ConstantBufferBinding cbuffer_binding_float_pixel_;
  ConstantBufferBinding cbuffer_binding_bool_loop_;
  ConstantBufferBinding cbuffer_binding_fetch_;
  ConstantBufferBinding cbuffer_binding_descriptor_indices_vertex_;
  ConstantBufferBinding cbuffer_binding_descriptor_indices_pixel_;

  /**
   * True when the latest shared memory and EDRAM binding holds the shared memory UAV rather than
   * the SRV. Separate descriptor tables for the SRV and UAV prevent a PIX validation message
   * about missing resource states, even though only 1 is accessed dynamically in shaders.
   */
  std::optional<bool> current_shared_memory_binding_is_uav_;

  u64 draw_view_bindful_heap_index_;
  u64 draw_sampler_bindful_heap_index_;

  /** True when the last used texture and sampler bindings are in the current descriptor heap. */
  bool bindful_textures_written_vertex_;
  bool bindful_textures_written_pixel_;
  bool bindful_samplers_written_vertex_;
  bool bindful_samplers_written_pixel_;
  /**
   * Layout UIDs and last texture and sampler bindings written to the current descriptor heaps
   * (bindful) or descriptor index constant buffer (bindless) with the last used layout. Valid
   * only when bindful_#_written_#_ is true for bindful, or
   * cbuffer_binding_descriptor_indices_#_.up_to_date is true for bindless.
   */
  size_t current_texture_layout_uid_vertex_;
  size_t current_texture_layout_uid_pixel_;
  size_t current_sampler_layout_uid_vertex_;
  size_t current_sampler_layout_uid_pixel_;
  /**
   * Sizes are ignored when checking whether these are up to date, as the layout UID differs for
   * different binding counts and is checked first.
   */
  std::vector<D3D12TextureCache::TextureSRVKey> current_texture_srv_keys_vertex_;
  std::vector<D3D12TextureCache::TextureSRVKey> current_texture_srv_keys_pixel_;
  std::vector<D3D12TextureCache::SamplerParameters> current_samplers_vertex_;
  std::vector<D3D12TextureCache::SamplerParameters> current_samplers_pixel_;
  std::vector<u32> current_sampler_bindless_indices_vertex_;
  std::vector<u32> current_sampler_bindless_indices_pixel_;

  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_shared_memory_srv_and_edram_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_shared_memory_uav_and_edram_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_textures_vertex_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_textures_pixel_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_samplers_vertex_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_samplers_pixel_;

  D3D_PRIMITIVE_TOPOLOGY primitive_topology_;

  std::vector<draw_state::MemExportRange> memexport_ranges_;
};

}  // namespace rex::gpu::d3d12
