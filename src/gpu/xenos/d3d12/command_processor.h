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
#include "gpu/xenos/d3d12/d3d12_xenos_graphics_driver.h"
#include "gpu/xenos/d3d12/pipeline_cache.h"
#include "gpu/xenos/d3d12/primitive_processor.h"
#include "gpu/xenos/d3d12/render_target_cache.h"
#include "gpu/xenos/d3d12/shared_memory.h"
#include "gpu/xenos/d3d12/texture_cache.h"
#include "gpu/xenos/pipeline/shader/dxbc.h"
#include "gpu/xenos/pipeline/shader/dxbc_translator.h"
#include <rex/xenos/register.h>
#include "gpu/xenos/draw_state.h"
#include <rex/xenos/format.h>
#include <rex/xenos/state.h>
#include "gpu/xenos/host/d3d12/d3d12_descriptor_heap_pool.h"
#include "gpu/xenos/host/d3d12/d3d12_provider.h"
#include "gpu/xenos/host/d3d12/d3d12_settings.h"
#include "gpu/xenos/host/d3d12/d3d12_upload_buffer_pool.h"
#include "gpu/xenos/host/d3d12/d3d12_objects.h"

namespace rex::gpu::d3d12 {

class D3D12CommandProcessor : public CommandProcessor {
 public:
  explicit D3D12CommandProcessor(D3D12XenosGraphicsDriver* graphics_system, const TitleInfo& info,
                                 IPhysicalMemory& memory, IPhysicalWriteWatch& watch,
                                 IGuestInterrupts& interrupts);
  ~D3D12CommandProcessor();

  void ClearCaches() override;
  void InvalidateGPUMemory() override;

  void InitializeShaderStorage(const std::filesystem::path& cache_root, u32 title_id,
                               bool blocking) override;

  const D3D12Settings& GetD3D12Settings() const;

  gpu::d3d12::D3D12Provider& GetD3D12Provider() const {
    return *static_cast<gpu::d3d12::D3D12Provider*>(graphics_system_->GetProvider());
  }

  DeferredCommandList& GetDeferredCommandList() {
    REX_ASSERT(submission_open_);
    return deferred_command_list_;
  }

  u64 GetCurrentSubmission() const { return submission_current_; }
  u64 GetCompletedSubmission() const { return submission_completed_; }

  /**
   * Tracks queue operations done outside submissions, such as UpdateTileMappings, so
   * CheckSubmissionFence(submission_current_) awaits them.
   */
  void NotifyQueueOperationsDoneDirectly() {
    queue_operations_done_since_submission_signal_ = true;
  }

  u64 GetCurrentFrame() const { return frame_current_; }
  u64 GetCompletedFrame() const { return frame_completed_; }

  /**
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
   * @param pixel_shader Pixel shader of the pipeline, may be null.
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
   * When the unbounded SRV range is used in the root signature of the referencing draw or dispatch,
   * it must only allocate SRVs, otherwise root signature creation fails on Nvidia Fermi.
   *
   * @return The descriptor index, or UINT32_MAX if no descriptors are free.
   */
  u32 RequestPersistentViewBindlessDescriptor();
  void ReleaseViewBindlessDescriptorImmediately(u32 descriptor_index) noexcept;
  /**
   * Requests non contiguous CBV, SRV and UAV descriptors for the next internal draw or dispatch
   * only. May change the current descriptor heap. The Nvidia Fermi rule of
   * RequestPersistentViewBindlessDescriptor applies.
   *
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
     * SRVs from here are accessible to shaders through an unbounded range. Descriptors of other
     * types must not follow here.
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
   * Returns a temporary GPU side buffer within a submission, for texture untiling and resolving.
   *
   * @param state State the buffer is in on return.
   */
  ID3D12Resource* RequestScratchGPUBuffer(u32 size, D3D12_RESOURCE_STATES state);
  /**
   * Releases the scratch buffer and sets its tracked state.
   *
   * @param buffer Buffer from RequestScratchGPUBuffer.
   */
  void ReleaseScratchGPUBuffer(ID3D12Resource* buffer, D3D12_RESOURCE_STATES new_state);

  /**
   * Returns a pipeline by its handle, waiting for deferred creation, or null when creation failed.
   */
  ID3D12PipelineState* GetD3D12PipelineByHandle(void* handle) const {
    return pipeline_cache_->GetD3D12PipelineByHandle(handle);
  }

  /**
   * Sets the cached state to external values, mainly for cache invalidation. A submission must be
   * open.
   */
  void SetExternalPipeline(ID3D12PipelineState* pipeline);
  void SetExternalGraphicsRootSignature(ID3D12RootSignature* root_signature);
  void SetViewport(const D3D12_VIEWPORT& viewport);
  void SetScissorRect(const D3D12_RECT& scissor_rect);
  void SetStencilReference(u32 stencil_ref);
  void SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY primitive_topology);

  /** Returns the GPU driver name text for the window title. */
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

  bool IssueDraw(xenos::PrimitiveType primitive_type, u32 index_count, IndexBuffer* index_buffer,
                 bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  static constexpr u32 kQueueFrames = 3;

  /**
   * Root parameters. The root signature at each stage must stay at 13 dwords or less (better 12)
   * to fit in AMD user data. Descriptor tables take 1 dword and root descriptors 2, though root
   * descriptors need less CPU setup. CBVs use root descriptors in both bindful and bindless cases.
   * Bindful uses multiple root signatures depending on extra parameters. Bindless uses two global
   * root signatures, for non tessellated and tessellated drawing.
   */
  enum RootParameter : u8 {
    /** Changes often, especially for UI draws and models drawn in parts. Holds fetch constants. */
    kRootParameterBindfulFetchConstants = 0, /**< +2 dwords = 2 in all. */
    /** Changes often, such as for an object drawn repeatedly. May hold projection matrices. */
    kRootParameterBindfulFloatConstantsVertex, /**< +2 = 4 in VS. */
    /** Changes less often, per material. */
    kRootParameterBindfulFloatConstantsPixel, /**< +2 = 4 in PS. */
    /** May stay the same across many draws. */
    kRootParameterBindfulSystemConstants,
    /** Rarely used and changed, holds flow control constants. */
    kRootParameterBindfulBoolLoopConstants,
    /**
     * Changes only when starting a new descriptor heap or switching shared memory between SRV
     * and UAV. Holds the shared memory byte address buffer (SRV and UAV, either may be null) and,
     * if ROV is used for EDRAM, the EDRAM R32_UINT UAV.
     */
    kRootParameterBindfulSharedMemoryAndEDRAM,

    kRootParameterBindfulCountBase,

    /**
     * Extra parameters that may exist are pixel textures (+1 = 10 in PS), pixel samplers (+1 = 11
     * in PS), vertex textures (+1 = 10 in VS) and vertex samplers (+1 = 11 in VS).
     */
    kRootParameterBindfulCountMax = kRootParameterBindfulCountBase + 4,

    kRootParameterBindlessFetchConstants = 0,
    kRootParameterBindlessFloatConstantsVertex, /**< +2 = 4 in VS. */
    kRootParameterBindlessFloatConstantsPixel,  /**< +2 = 4 in PS. */
    /** Texture and sampler descriptor indices, changed per material. */
    kRootParameterBindlessDescriptorIndicesPixel,  /**< +2 = 6 in PS. */
    kRootParameterBindlessDescriptorIndicesVertex, /**< +2 = 6 in VS. */
    kRootParameterBindlessSystemConstants,
    kRootParameterBindlessBoolLoopConstants,
    /**
     * Changes only when switching shared memory between SRV and UAV. Holds the shared memory
     * byte address buffer (SRV and UAV, either may be null).
     */
    kRootParameterBindlessSharedMemory,
    /** Unbounded sampler descriptor table, changed in case of overflow. */
    kRootParameterBindlessSamplerHeap,
    /** Unbounded SRV and UAV descriptor table, never changed. */
    kRootParameterBindlessViewHeap,

    kRootParameterBindlessCount,
  };

  struct RootBindfulExtraParameterIndices {
    u32 textures_pixel;
    u32 samplers_pixel;
    u32 textures_vertex;
    u32 samplers_vertex;
    static constexpr u32 kUnavailable = UINT32_MAX;
  };
  /**
   * Fills the indices of the optional root parameters.
   *
   * @param pixel_shader The pixel shader, may be null.
   * @param indices_out Receives the indices.
   * @return The total parameter count.
   */
  static u32 GetRootBindfulExtraParameterIndices(const DXBCShader* vertex_shader,
                                                 const DXBCShader* pixel_shader,
                                                 RootBindfulExtraParameterIndices& indices_out);

  /**
   * Rechecks the submission number and reclaims per submission resources.
   *
   * @param await_submission The submission to wait for, 0 to check the status, or
   * submission_current_ to wait for all queue operations.
   */
  void CheckSubmissionFence(u64 await_submission);
  /**
   * Begins a submission, promoting an open non frame submission to a frame when is_guest_command
   * is true. Submission boundaries are implicit full UAV and aliasing barriers and cause common
   * resource state promotion and decay.
   *
   * @param is_guest_command True when the submission carries a guest command.
   * @return True when a submission is open and the device is not removed.
   */
  bool BeginSubmission(bool is_guest_command);
  /**
   * Ends a submission. On failure the submission stays open.
   *
   * @param is_swap True to close the frame whether or not the submission closes.
   */
  bool EndSubmission(bool is_swap);
  /**
   * Reports whether ending the submission now costs no more delay than it saves, as unfinished
   * pipeline creation requests must complete before submitting.
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
  bool AreDebugMarkersEnabled() const { return debug_markers_enabled_; }

  /** Submission completion must be awaited first. */
  void ClearCommandAllocatorCache();

  /**
   * Requests descriptors and rebinds the descriptor heap on the draw command list. See
   * D3D12DescriptorHeapPool::Request for the partial and full update counts. Not for bindless
   * descriptors.
   *
   * @param cpu_handle_out Receives the CPU handle of the descriptors.
   * @param gpu_handle_out Receives the GPU handle of the descriptors.
   */
  u64 RequestViewBindfulDescriptors(u64 previous_heap_index, u32 count_for_partial_update,
                                    u32 count_for_full_update,
                                    D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out,
                                    D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out);
  u64 RequestSamplerBindfulDescriptors(u64 previous_heap_index, u32 count_for_partial_update,
                                       u32 count_for_full_update,
                                       D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out,
                                       D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out);

  void UpdateFixedFunctionState(const draw_state::HostViewport& host_viewport,
                                const draw_state::Scissor& scissor, bool primitive_polygonal,
                                xenos::reg::RB_DEPTHCONTROL normalized_depth_control);
  void UpdateSystemConstantValues(bool shared_memory_is_uav, bool primitive_polygonal,
                                  u32 line_loop_closing_index, xenos::Endian index_endian,
                                  const draw_state::HostViewport& host_viewport,
                                  u32 used_texture_mask,
                                  xenos::reg::RB_DEPTHCONTROL normalized_depth_control,
                                  u32 normalized_color_mask);
  bool UpdateBindings(const D3D12Shader* vertex_shader, const D3D12Shader* pixel_shader,
                      ID3D12RootSignature* root_signature, bool shared_memory_is_uav);
  bool IssueCopyReadbackResolvePath();
  bool IssueDrawMemexportReadbackFullPath(u32 total_size);
  bool IssueDrawMemexportReadbackFastPath(u32 total_size);

  /** Returns a COPY_DEST buffer for reading GPU data back, synchronized right after use. */
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

  bool InitializeOcclusionQueryResources();
  void ShutdownOcclusionQueryResources();
  bool BeginGuestOcclusionQuery(u32 sample_count_address);
  bool EndGuestOcclusionQuery(xenos::DepthSampleCounts* sample_counts);
  void DisableHostOcclusionQueries();
  u64 NormalizeOcclusionSamples(u64 samples) const;
  void WriteGuestOcclusionResult(xenos::DepthSampleCounts* sample_counts, u64 samples);

  void WriteGammaRampSRV(bool is_pwl, D3D12_CPU_DESCRIPTOR_HANDLE handle) const;

  bool device_removed_ = false;

  bool cache_clear_requested_ = false;

  HANDLE fence_completion_event_ = nullptr;

  bool submission_open_ = false;
  /** Values of submission_fence_. */
  u64 submission_current_ = 1;
  u64 submission_completed_ = 0;
  ID3D12Fence* submission_fence_ = nullptr;

  /** Awaits queue operations such as UpdateTileMappings queued after the latest Signal. */
  ID3D12Fence* queue_operations_since_submission_fence_ = nullptr;
  u64 queue_operations_since_submission_fence_last_ = 0;
  bool queue_operations_done_since_submission_signal_ = false;

  bool frame_open_ = false;
  /** Title frame index, updated at the start of a frame. */
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

  /** Skips GetHostViewport when the affecting registers are unchanged. */
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
  draw_state::HostViewport previous_host_viewport_{};
  bool viewport_cache_valid_ = false;

  /** Bindless is much faster than bindful, where CopyDescriptorsSimple dominates UpdateBindings. */
  bool bindless_resources_used_ = false;

  std::unique_ptr<D3D12SharedMemory> shared_memory_;

  std::unique_ptr<D3D12RenderTargetCache> render_target_cache_;

  static constexpr u32 kWindowTitleStateValid = u32{1} << 31;
  std::atomic<u32> window_title_state_{0};

  std::unique_ptr<gpu::d3d12::D3D12UploadBufferPool> constant_buffer_pool_;

  static constexpr u32 kViewBindfulHeapSize = 32768;
  static_assert(kViewBindfulHeapSize <= D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1);
  std::unique_ptr<gpu::d3d12::D3D12DescriptorHeapPool> view_bindful_heap_pool_;
  ID3D12DescriptorHeap* view_bindful_heap_current_;
  /**
   * Textures have 4 KB alignment in guest memory, so at most 512 MB / 4 KB exist, times 3 for
   * swizzles, signedness, multiple host textures per guest texture and transient descriptors.
   * The limit is 1000000 for resource binding tier 2.
   */
  static constexpr u32 kViewBindlessHeapSize = 262144;
  static_assert(kViewBindlessHeapSize <= D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_2);
  ID3D12DescriptorHeap* view_bindless_heap_ = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE view_bindless_heap_cpu_start_;
  D3D12_GPU_DESCRIPTOR_HANDLE view_bindless_heap_gpu_start_;
  u32 view_bindless_heap_allocated_ = 0;
  std::vector<u32> view_bindless_heap_free_;
  u32 view_bindless_heap_free_count_ = 0;
  /** Descriptor index and requesting submission pairs, sorted by submission. */
  std::deque<std::pair<u32, u64>> view_bindless_one_use_descriptors_;

  /**
   * Shader visible heaps hold at most 2048 samplers.
   */
  static constexpr u32 kSamplerHeapSize = 2000;
  static_assert(kSamplerHeapSize <= D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE);
  std::unique_ptr<gpu::d3d12::D3D12DescriptorHeapPool> sampler_bindful_heap_pool_;
  ID3D12DescriptorHeap* sampler_bindful_heap_current_;
  ID3D12DescriptorHeap* sampler_bindless_heap_current_ = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE sampler_bindless_heap_cpu_start_;
  D3D12_GPU_DESCRIPTOR_HANDLE sampler_bindless_heap_gpu_start_;
  u32 sampler_bindless_heap_allocated_ = 0;
  /**
   * Heap and overflow submission pairs, kept when the sampler count exceeds kSamplerHeapSize and
   * the heap is switched. Direct3D 9 sampler parameter state means one changed parameter may double
   * the sampler count. Sorted by submission, so checking the first for reuse is enough.
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
  /** Like gamma_ramp_buffer_ as an image with kQueueFrames array layers. */
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

  struct FXAAConstants {
    u32 size[2];
    f32 size_inv[2];
  };
  enum class FXAARootParameter : u8 {
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
   * The PWL gamma ramp can produce more than 10 bits per channel. The FXAA shader needs luma in
   * alpha, so R16G16B16A16_UNORM keeps all 13 bits generated with an increment of 2^3 and leaves
   * room for fractional luma weights.
   */
  static constexpr DXGI_FORMAT kFXAASourceTextureFormat = DXGI_FORMAT_R16G16B16A16_UNORM;
  /** Kept in NON_PIXEL_SHADER_RESOURCE state. */
  Microsoft::WRL::ComPtr<ID3D12Resource> fxaa_source_texture_;
  u64 fxaa_source_texture_submission_ = 0;

  std::vector<D3D12_RESOURCE_BARRIER> barriers_;

  /** Requesting submission and resource pairs, sorted by submission. */
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

  Microsoft::WRL::ComPtr<ID3D12QueryHeap> occlusion_query_heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> occlusion_query_readback_;
  u64* occlusion_query_readback_mapping_ = nullptr;
  bool occlusion_query_resources_available_ = false;
  struct ActiveOcclusionQuery {
    u32 sample_count_address = 0;
    u32 host_index = UINT32_MAX;
    bool valid = false;
  } active_occlusion_query_;

  D3D12_VIEWPORT ff_viewport_;
  D3D12_RECT ff_scissor_;
  f32 ff_blend_factor_[4];
  u32 ff_stencil_ref_;
  bool ff_viewport_update_needed_;
  bool ff_scissor_update_needed_;
  bool ff_blend_factor_update_needed_;
  bool ff_stencil_ref_update_needed_;

  /**
   * The bound pipeline is either a pipeline cache graphics pipeline, possibly with deferred
   * creation, with current_external_pipeline_ null, or a non Xenos graphics or compute pipeline,
   * with current_guest_pipeline_ null.
   */
  void* current_guest_pipeline_;
  ID3D12PipelineState* current_external_pipeline_;

  ID3D12RootSignature* current_graphics_root_signature_;
  RootBindfulExtraParameterIndices current_graphics_root_bindful_extras_;
  /** Bits of the up to date root parameters, reset when a new signature is bound. */
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
   * True when the latest shared memory and EDRAM binding holds the shared memory UAV, not the SRV.
   * Separate SRV and UAV descriptor tables prevent a PIX validation message about missing resource
   * states, though only one is accessed dynamically in shaders.
   */
  std::optional<bool> current_shared_memory_binding_is_uav_;

  u64 draw_view_bindful_heap_index_;
  u64 draw_sampler_bindful_heap_index_;

  /** True when the last texture and sampler bindings are in the current descriptor heap. */
  bool bindful_textures_written_vertex_;
  bool bindful_textures_written_pixel_;
  bool bindful_samplers_written_vertex_;
  bool bindful_samplers_written_pixel_;
  /**
   * Layout UIDs and the last texture and sampler bindings written to the current descriptor heaps
   * (bindful) or descriptor index constant buffer (bindless). Valid only when
   * bindful_#_written_#_ (bindful) or cbuffer_binding_descriptor_indices_#_.up_to_date (bindless)
   * is true.
   */
  size_t current_texture_layout_uid_vertex_;
  size_t current_texture_layout_uid_pixel_;
  size_t current_sampler_layout_uid_vertex_;
  size_t current_sampler_layout_uid_pixel_;
  /**
   * Sizes are ignored when checking whether these are up to date, as the layout UID is checked
   * first and differs for different binding counts.
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
