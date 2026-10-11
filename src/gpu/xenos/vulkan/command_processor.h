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

#include <array>
#include <climits>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/types.h>
#include "gpu/xenos/command_processor.h"
#include "gpu/xenos/pipeline/shader/spirv_translator.h"
#include <rex/xenos/registers.h>
#include "gpu/xenos/draw_state.h"
#include "gpu/xenos/vulkan/deferred_command_buffer.h"
#include "gpu/xenos/vulkan/graphics_system.h"
#include "gpu/xenos/vulkan/pipeline_cache.h"
#include "gpu/xenos/vulkan/primitive_processor.h"
#include "gpu/xenos/vulkan/render_target_cache.h"
#include "gpu/xenos/vulkan/shader.h"
#include "gpu/xenos/vulkan/shared_memory.h"
#include "gpu/xenos/vulkan/texture_cache.h"
#include <rex/xenos/xenos.h>
#include <rex/hash.h>
#include <rex/runtime.h>
#include "gpu/xenos/host/vulkan/linked_type_descriptor_set_allocator.h"
#include "gpu/xenos/host/vulkan/presenter.h"
#include "gpu/xenos/host/vulkan/provider.h"
#include "gpu/xenos/host/vulkan/upload_buffer_pool.h"

namespace rex::gpu::vulkan {

class VulkanCommandProcessor : public CommandProcessor {
 public:
  enum class SingleTransientDescriptorLayout : u8 {
    kStorageBufferCompute,
    kStorageBufferPairCompute,
    kCount,
  };

  class ScratchBufferAcquisition : public rex::MoveOnly {
   public:
    explicit ScratchBufferAcquisition() = default;
    explicit ScratchBufferAcquisition(VulkanCommandProcessor& command_processor, VkBuffer buffer,
                                      VkPipelineStageFlags stage_mask, VkAccessFlags access_mask)
        : command_processor_(&command_processor),
          buffer_(buffer),
          stage_mask_(stage_mask),
          access_mask_(access_mask) {}

    ScratchBufferAcquisition(ScratchBufferAcquisition&& acquisition) noexcept
        : command_processor_(std::exchange(acquisition.command_processor_, nullptr)),
          buffer_(std::exchange(acquisition.buffer_, VK_NULL_HANDLE)),
          stage_mask_(std::exchange(acquisition.stage_mask_, 0)),
          access_mask_(std::exchange(acquisition.access_mask_, 0)) {}
    ScratchBufferAcquisition& operator=(ScratchBufferAcquisition&& acquisition) noexcept {
      if (this == &acquisition) {
        return *this;
      }
      Release();
      command_processor_ = std::exchange(acquisition.command_processor_, nullptr);
      buffer_ = std::exchange(acquisition.buffer_, VK_NULL_HANDLE);
      stage_mask_ = std::exchange(acquisition.stage_mask_, 0);
      access_mask_ = std::exchange(acquisition.access_mask_, 0);
      return *this;
    }

    ~ScratchBufferAcquisition() { Release(); }

    /**
     * Returns the buffer.
     *
     * @return The buffer, or VK_NULL_HANDLE when acquisition failed or this was moved from.
     */
    VkBuffer buffer() const { return buffer_; }

    VkPipelineStageFlags GetStageMask() const { return stage_mask_; }
    VkPipelineStageFlags SetStageMask(VkPipelineStageFlags new_stage_mask) {
      VkPipelineStageFlags old_stage_mask = stage_mask_;
      stage_mask_ = new_stage_mask;
      return old_stage_mask;
    }
    VkAccessFlags GetAccessMask() const { return access_mask_; }
    VkAccessFlags SetAccessMask(VkAccessFlags new_access_mask) {
      VkAccessFlags old_access_mask = access_mask_;
      access_mask_ = new_access_mask;
      return old_access_mask;
    }

   private:
    void Release() noexcept {
      if (buffer_ == VK_NULL_HANDLE) {
        return;
      }
      REX_ASSERT(command_processor_->scratch_buffer_used_);
      REX_ASSERT(command_processor_->scratch_buffer_ == buffer_);
      command_processor_->scratch_buffer_last_stage_mask_ = stage_mask_;
      command_processor_->scratch_buffer_last_access_mask_ = access_mask_;
      command_processor_->scratch_buffer_last_usage_submission_ =
          command_processor_->GetCurrentSubmission();
      command_processor_->scratch_buffer_used_ = false;
      buffer_ = VK_NULL_HANDLE;
    }

    VulkanCommandProcessor* command_processor_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkPipelineStageFlags stage_mask_ = 0;
    VkAccessFlags access_mask_ = 0;
  };

  VulkanCommandProcessor(VulkanGraphicsSystem* graphics_system, Runtime& runtime);
  ~VulkanCommandProcessor();

  void ClearCaches() override;
  void InvalidateGPUMemory() override;
  void InitializeShaderStorage(const std::filesystem::path& cache_root, u32 title_id,
                               bool blocking) override;

  gpu::vulkan::VulkanDevice* GetVulkanDevice() const {
    return static_cast<const gpu::vulkan::VulkanProvider*>(graphics_system_->provider())
        ->vulkan_device();
  }

  bool CompileGLSLToSPIRV(VkShaderStageFlagBits stage, std::string_view source,
                          std::vector<u32>& spirv_out, std::string& error_out) const;

  /**
   * Returns the deferred drawing command list for the open submission.
   *
   * @return The command buffer.
   */
  DeferredCommandBuffer& deferred_command_buffer() {
    REX_ASSERT(submission_open_);
    return deferred_command_buffer_;
  }

  bool submission_open() const { return submission_open_; }
  u64 GetCurrentSubmission() const {
    return submission_completed_ + static_cast<u64>(submissions_in_flight_fences_.size()) + 1;
  }
  u64 GetCompletedSubmission() const { return submission_completed_; }

  /**
   * Queues sparse binds. A submission sends them in one vkQueueBindSparse without waiting for a
   * semaphore, so binds between the completed and current submission and within one submission
   * must not touch overlapping memory.
   *
   * @param buffer Buffer to bind.
   * @param bind_count Number of binds.
   * @param binds Sparse memory binds.
   * @param wait_stage_mask Pipeline stages that wait for the binds.
   */
  void SparseBindBuffer(VkBuffer buffer, u32 bind_count, const VkSparseMemoryBind* binds,
                        VkPipelineStageFlags wait_stage_mask);

  u64 GetCurrentFrame() const { return frame_current_; }
  u64 GetCompletedFrame() const { return frame_completed_; }

  /**
   * Inserts a buffer memory barrier. The submission must be open. Stage masks are 0 when no
   * pipeline stage accesses the resource in a synchronization scope.
   *
   * @param buffer Buffer to protect.
   * @param offset Range start in bytes.
   * @param size Range size in bytes.
   * @param src_stage_mask Source pipeline stages.
   * @param dst_stage_mask Destination pipeline stages.
   * @param src_access_mask Source access.
   * @param dst_access_mask Destination access.
   * @param src_queue_family_index Source queue family.
   * @param dst_queue_family_index Destination queue family.
   * @param skip_if_equal Whether to drop the barrier when source and destination are equal.
   * @return True when the barrier was inserted and not dropped.
   */
  bool PushBufferMemoryBarrier(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
                               VkPipelineStageFlags src_stage_mask,
                               VkPipelineStageFlags dst_stage_mask, VkAccessFlags src_access_mask,
                               VkAccessFlags dst_access_mask,
                               u32 src_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                               u32 dst_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                               bool skip_if_equal = true);
  /**
   * Inserts an image memory barrier, with the same rules as PushBufferMemoryBarrier.
   *
   * @param image Image to protect.
   * @param subresource_range Subresource range.
   * @param src_stage_mask Source pipeline stages.
   * @param dst_stage_mask Destination pipeline stages.
   * @param src_access_mask Source access.
   * @param dst_access_mask Destination access.
   * @param old_layout Layout before the barrier.
   * @param new_layout Layout after the barrier.
   * @param src_queue_family_index Source queue family.
   * @param dst_queue_family_index Destination queue family.
   * @param skip_if_equal Whether to drop the barrier when source and destination are equal.
   * @return True when the barrier was inserted and not dropped.
   */
  bool PushImageMemoryBarrier(VkImage image, const VkImageSubresourceRange& subresource_range,
                              VkPipelineStageFlags src_stage_mask,
                              VkPipelineStageFlags dst_stage_mask, VkAccessFlags src_access_mask,
                              VkAccessFlags dst_access_mask, VkImageLayout old_layout,
                              VkImageLayout new_layout,
                              u32 src_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                              u32 dst_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                              bool skip_if_equal = true);
  /**
   * Submits the pending barriers.
   *
   * @param force_end_render_pass Whether to end the render pass even without barriers.
   * @return True when barriers were submitted, which also closes the render pass.
   */
  bool SubmitBarriers(bool force_end_render_pass);

  /**
   * Begins a render pass from the render target cache if not started yet. The submission must be
   * open.
   *
   * @param render_pass Render pass to enter.
   * @param framebuffer Framebuffer for the render pass.
   */
  void SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      VkRenderPass render_pass, const VulkanRenderTargetCache::Framebuffer* framebuffer);
  void SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      VkRenderPass render_pass, const VulkanRenderTargetCache::Framebuffer* framebuffer,
      VkImageView transfer_dest_view, bool transfer_dest_is_depth);
  /**
   * Ends the render pass. Needed before anything outside the render pass scope, including
   * pipeline barriers that are not part of it. The submission must be open.
   */
  void EndRenderPass();

  VkDescriptorSetLayout GetSingleTransientDescriptorLayout(
      SingleTransientDescriptorLayout transient_descriptor_layout) const {
    return descriptor_set_layouts_single_transient_[static_cast<size_t>(
        transient_descriptor_layout)];
  }
  // A frame must be open.
  VkDescriptorSet AllocateSingleTransientDescriptor(
      SingleTransientDescriptorLayout transient_descriptor_layout);

  // The returned reference is valid until a cache clear.
  VkDescriptorSetLayout GetTextureDescriptorSetLayout(bool is_vertex, size_t texture_count,
                                                      size_t sampler_count);
  // The returned reference is valid until a cache clear.
  const VulkanPipelineCache::PipelineLayoutProvider* GetPipelineLayout(size_t texture_count_pixel,
                                                                       size_t sampler_count_pixel,
                                                                       size_t texture_count_vertex,
                                                                       size_t sampler_count_vertex);

  /**
   * Returns a temporary GPU side buffer within a submission for tasks like texture untiling and
   * resolving. May push a buffer memory barrier into the initial usage. The submission must be
   * open.
   *
   * @param size Buffer size in bytes.
   * @param initial_stage_mask Pipeline stages of the initial usage.
   * @param initial_access_mask Access of the initial usage.
   * @return The acquisition, with a null buffer on failure.
   */
  ScratchBufferAcquisition AcquireScratchGPUBuffer(VkDeviceSize size,
                                                   VkPipelineStageFlags initial_stage_mask,
                                                   VkAccessFlags initial_access_mask);

  /**
   * Binds a graphics pipeline for host specific purposes, invalidating the affected state.
   * Keep flags are false after binding a pipeline with static state or when the caller bypasses
   * VulkanCommandProcessor tracking, and true only when the caller modifies the dynamic state
   * through that tracking.
   *
   * @param pipeline Pipeline to bind.
   * @param keep_dynamic_depth_bias True to keep the tracked dynamic depth bias.
   * @param keep_dynamic_blend_constants True to keep the tracked dynamic blend constants.
   * @param keep_dynamic_stencil_mask_ref True to keep the tracked stencil masks and references.
   */
  void BindExternalGraphicsPipeline(VkPipeline pipeline, bool keep_dynamic_depth_bias = false,
                                    bool keep_dynamic_blend_constants = false,
                                    bool keep_dynamic_stencil_mask_ref = false);
  void BindExternalComputePipeline(VkPipeline pipeline);
  void SetViewport(const VkViewport& viewport);
  void SetScissor(const VkRect2D& scissor);

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  void WriteRegister(u32 index, u32 value) override;
  void WriteRegistersFromMem(u32 start_index, u32* base, u32 num_registers) override;
  bool ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader, u32 packet,
                                          u32 count) override;

  void OnGammaRamp256EntryTableValueWritten() override;
  void OnGammaRampPWLValueWritten() override;
  void OnPrimaryBufferEnd() override;

  void IssueSwap(u32 frontbuffer_ptr, u32 frontbuffer_width, u32 frontbuffer_height) override;

  Shader* LoadShader(xenos::ShaderType shader_type, u32 guest_address, const u32* host_address,
                     u32 dword_count) override;

  /**
   * Issues a draw, running up to 2 iterations of the subsystem updates. A sampler overflow may
   * require awaiting a submission, possibly the current one, and earlier updates may then
   * reference objects destroyed by CompletedSubmissionUpdated.
   *
   * @param prim_type Primitive type of the draw.
   * @param index_count Number of vertices or indices.
   * @param index_buffer_info Index buffer description, or null for auto indexed draws.
   * @param major_mode_explicit Whether the major mode was set explicitly.
   * @return False when a sampler could not be created, or overflowed twice because of too many
   *         unique samplers in 1 draw or a failed await.
   */
  bool IssueDraw(xenos::PrimitiveType prim_type, u32 index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  struct CommandBuffer {
    VkCommandPool pool;
    VkCommandBuffer buffer;
  };

  struct SparseBufferBind {
    VkBuffer buffer;
    size_t bind_offset;
    u32 bind_count;
  };

  union TextureDescriptorSetLayoutKey {
    u32 key;
    struct {
      /** With both counts 0, descriptor_set_layout_empty_ is used instead, these own references. */
      u32 texture_count : 16;
      u32 sampler_count : 15;
      u32 is_vertex : 1;
    };

    TextureDescriptorSetLayoutKey() : key(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const TextureDescriptorSetLayoutKey& key) const {
        return std::hash<decltype(key.key)>{}(key.key);
      }
    };
    bool operator==(const TextureDescriptorSetLayoutKey& other_key) const {
      return key == other_key.key;
    }
    bool operator!=(const TextureDescriptorSetLayoutKey& other_key) const {
      return !(*this == other_key);
    }
  };

  union PipelineLayoutKey {
    u64 key;
    struct {
      // Pixel textures occupy the low bits because they vary more often.
      u16 texture_count_pixel;
      u16 sampler_count_pixel;
      u16 texture_count_vertex;
      u16 sampler_count_vertex;
    };

    PipelineLayoutKey() : key(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const PipelineLayoutKey& key) const {
        return std::hash<decltype(key.key)>{}(key.key);
      }
    };
    bool operator==(const PipelineLayoutKey& other_key) const { return key == other_key.key; }
    bool operator!=(const PipelineLayoutKey& other_key) const { return !(*this == other_key); }
  };

  class PipelineLayout : public VulkanPipelineCache::PipelineLayoutProvider {
   public:
    explicit PipelineLayout(VkPipelineLayout pipeline_layout,
                            VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref,
                            VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref)
        : pipeline_layout_(pipeline_layout),
          descriptor_set_layout_textures_vertex_ref_(descriptor_set_layout_textures_vertex_ref),
          descriptor_set_layout_textures_pixel_ref_(descriptor_set_layout_textures_pixel_ref) {}
    VkPipelineLayout GetPipelineLayout() const override { return pipeline_layout_; }
    VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref() const {
      return descriptor_set_layout_textures_vertex_ref_;
    }
    VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref() const {
      return descriptor_set_layout_textures_pixel_ref_;
    }

   private:
    VkPipelineLayout pipeline_layout_;
    VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref_;
    VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref_;
  };

  struct UsedSingleTransientDescriptor {
    u64 frame;
    SingleTransientDescriptorLayout layout;
    VkDescriptorSet set;
  };

  struct UsedTextureTransientDescriptorSet {
    u64 frame;
    TextureDescriptorSetLayoutKey layout;
    VkDescriptorSet set;
  };

  enum SwapApplyGammaDescriptorSet : u8 {
    kSwapApplyGammaDescriptorSetRamp,
    kSwapApplyGammaDescriptorSetSource,

    kSwapApplyGammaDescriptorSetCount,
  };
  enum SwapApplyGammaComputeDescriptorSet : u8 {
    kSwapApplyGammaComputeDescriptorSetRamp,
    kSwapApplyGammaComputeDescriptorSetSource,
    kSwapApplyGammaComputeDescriptorSetDestination,

    kSwapApplyGammaComputeDescriptorSetCount,
  };
  struct SwapApplyGammaConstants {
    u32 size[2];
  };
  enum SwapFxaaDescriptorSet : u8 {
    kSwapFxaaDescriptorSetSource,
    kSwapFxaaDescriptorSetDestination,

    kSwapFxaaDescriptorSetCount,
  };
  struct SwapFxaaConstants {
    u32 size[2];
    f32 size_inv[2];
  };

  /** Framebuffer for the current presenter guest output image revision, with usage tracking. */
  struct SwapFramebuffer {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    u64 version = UINT64_MAX;
    u64 last_submission = 0;
  };

  /**
   * Rechecks the submission number and reclaims per submission resources.
   *
   * @param await_submission Submission to await, 0 only checks status and GetCurrentSubmission()
   *        waits for all queue operations to complete.
   */
  void CheckSubmissionFenceAndDeviceLoss(u64 await_submission);
  /**
   * Opens a submission. May be called at any time. An open non frame submission is promoted to a
   * frame when is_guest_command is true. Unlike Direct3D 12, submission boundaries imply no memory
   * barriers aside from an incoming host write dependency (outgoing host reads are not covered).
   *
   * A guest command opens a new full frame with resource cleanup and a pending capture start,
   * instead of resuming after mid frame synchronization.
   *
   * @param is_guest_command True for a guest command.
   * @return True when a submission is open and the device is not lost.
   */
  bool BeginSubmission(bool is_guest_command);
  /**
   * Closes a submission. May be called at any time.
   *
   * A swap closes the full frame with cache clearing and a pending capture stop, whether or not
   * the submission is closed.
   *
   * @param is_swap True for a swap.
   * @return True when the submission completed, otherwise it stays open.
   */
  bool EndSubmission(bool is_swap);
  bool AwaitAllQueueOperationsCompletion() {
    CheckSubmissionFenceAndDeviceLoss(GetCurrentSubmission());
    return !submission_open_ && submissions_in_flight_fences_.empty();
  }
  /**
   * Reports whether a submission can end immediately at the primary buffer end.
   *
   * @return True when immediate submission is safe.
   */
  bool CanEndSubmissionImmediately() const;

  void ClearTransientDescriptorPools();
  bool IssueCopy_ReadbackResolvePath();
  bool IssueDraw_MemexportReadbackFullPath(u32 total_size);
  bool IssueDraw_MemexportReadbackFastPath(u32 total_size);

  void SplitPendingBarrier();

  void DestroyScratchBuffer();
  bool InitializeOcclusionQueryResources();
  void ShutdownOcclusionQueryResources();
  bool BeginGuestOcclusionQuery(u32 sample_count_address);
  bool EndGuestOcclusionQuery(u32 sample_count_address);
  bool AcquireOcclusionQueryIndex(u32& host_index_out);
  void DisableHostOcclusionQueries();
  u64 NormalizeOcclusionSamples(u64 samples) const;
  void WriteGuestOcclusionResult(u32 sample_count_address, u64 samples);
  void InvalidateAllVertexBufferResidency();
  void InvalidateVertexBufferResidency(u32 vfetch_index);
  void InvalidateVertexBufferResidencyRange(u32 first_vfetch, u32 last_vfetch);
  struct ReadbackBuffer {
    VkBuffer buffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory memories[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    void* mapped_data[2] = {nullptr, nullptr};
    u32 sizes[2] = {0, 0};
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
  struct ResolveDownscaleConstants {
    u32 scale_x;
    u32 scale_y;
    u32 pixel_size_log2;
    u32 tile_count;
    u32 source_offset_bytes;
    u32 half_pixel_offset;
  };
  bool EnsureSwapFxaaSourceImage(u32 width, u32 height);
  void DestroySwapFxaaSourceImage();

  void UpdateDynamicState(const draw_state::ViewportInfo& viewport_info, bool primitive_polygonal,
                          xenos::reg::RB_DEPTHCONTROL normalized_depth_control);
  /**
   * Updates the system constants. W0 division follows PA_CL_VTE_CNTL. Bit 8 VTX_XY_FMT true
   * means incoming XY are already multiplied by 1/W0, false means multiply. Bit 9 VTX_Z_FMT is
   * the same for Z. Bit 10 VTX_W0_FMT true means the incoming W0 is not 1/W0, so take the
   * reciprocal.
   *
   * @param primitive_polygonal Whether the primitive is polygonal.
   * @param primitive_processing_result Result of primitive processing.
   * @param shader_32bit_index_dma Whether the shader loads 32 bit indices itself.
   * @param compute_memexport_vertex_count Vertex count for memexport compute draws.
   * @param viewport_info Host viewport information.
   * @param used_texture_mask Mask of used texture fetch constants.
   * @param normalized_depth_control Normalized RB_DEPTHCONTROL.
   * @param normalized_color_mask Normalized color write mask.
   */
  void UpdateSystemConstantValues(
      bool primitive_polygonal,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      bool shader_32bit_index_dma, u32 compute_memexport_vertex_count,
      const draw_state::ViewportInfo& viewport_info, u32 used_texture_mask,
      xenos::reg::RB_DEPTHCONTROL normalized_depth_control, u32 normalized_color_mask);
  bool UpdateBindings(const VulkanShader* vertex_shader, const VulkanShader* pixel_shader);
  /**
   * Allocates a descriptor set and fills 1 or 2 VkWriteDescriptorSet entries for images and
   * samplers.
   *
   * @param is_vertex Whether the bindings are for the vertex stage.
   * @param texture_count Number of texture bindings.
   * @param sampler_count Number of sampler bindings.
   * @param descriptor_set_layout GetTextureDescriptorSetLayout result for the 3 arguments above.
   * @param texture_image_info Texture descriptors.
   * @param sampler_image_info Sampler descriptors.
   * @param descriptor_set_writes_out Receives the writes.
   * @return The number of writes, or 0 on allocation failure or when no bindings were requested.
   */
  u32 WriteTransientTextureBindings(bool is_vertex, u32 texture_count, u32 sampler_count,
                                    VkDescriptorSetLayout descriptor_set_layout,
                                    const VkDescriptorImageInfo* texture_image_info,
                                    const VkDescriptorImageInfo* sampler_image_info,
                                    VkWriteDescriptorSet* descriptor_set_writes_out);

  bool device_lost_ = false;

  bool cache_clear_requested_ = false;

  VkPipelineStageFlags guest_shader_pipeline_stages_ = 0;
  VkShaderStageFlags guest_shader_vertex_stages_ = 0;

  std::vector<VkFence> fences_free_;
  std::vector<VkSemaphore> semaphores_free_;

  bool submission_open_ = false;
  u64 submission_completed_ = 0;
  /** Kept so the next attempt waits correctly when vkQueueSubmit fails after a bind succeeded. */
  std::vector<VkSemaphore> current_submission_wait_semaphores_;
  std::vector<VkPipelineStageFlags> current_submission_wait_stage_masks_;
  std::vector<VkFence> submissions_in_flight_fences_;
  std::deque<std::pair<u64, VkSemaphore>> submissions_in_flight_semaphores_;

  static constexpr u32 kMaxFramesInFlight = 3;
  bool frame_open_ = false;
  bool frame_used_async_placeholder_pipeline_ = false;
  u64 frame_current_ = 1;
  u64 frame_completed_ = 0;
  u64 closed_frame_submissions_[kMaxFramesInFlight] = {};

  /** Pairs of last used submission and resource, sorted by submission. */
  std::deque<std::pair<u64, VkDeviceMemory>> destroy_memory_;
  std::deque<std::pair<u64, VkBuffer>> destroy_buffers_;
  std::deque<std::pair<u64, VkImageView>> destroy_image_views_;
  std::deque<std::pair<u64, VkImage>> destroy_images_;
  std::deque<std::pair<u64, VkFramebuffer>> destroy_framebuffers_;

  std::vector<CommandBuffer> command_buffers_writable_;
  std::deque<std::pair<u64, CommandBuffer>> command_buffers_submitted_;
  DeferredCommandBuffer deferred_command_buffer_;

  std::vector<VkSparseMemoryBind> sparse_memory_binds_;
  std::vector<SparseBufferBind> sparse_buffer_binds_;
  /** Filled from SparseBufferBind on submission because pBinds must point into a stable vector. */
  std::vector<VkSparseBufferMemoryBindInfo> sparse_buffer_bind_infos_temp_;
  VkPipelineStageFlags sparse_bind_wait_stage_mask_ = 0;

  std::vector<VkDescriptorSetLayoutBinding> descriptor_set_layout_bindings_;
  std::vector<VkDescriptorImageInfo> descriptor_write_image_info_;

  std::unique_ptr<gpu::vulkan::VulkanUploadBufferPool> uniform_buffer_pool_;

  VkDescriptorSetLayout descriptor_set_layout_empty_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_constants_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSetLayout, static_cast<size_t>(SingleTransientDescriptorLayout::kCount)>
      descriptor_set_layouts_single_transient_{};
  VkDescriptorSetLayout descriptor_set_layout_shared_memory_and_edram_ = VK_NULL_HANDLE;

  /** Referenced by pipeline_layouts_. */
  std::unordered_map<TextureDescriptorSetLayoutKey, VkDescriptorSetLayout,
                     TextureDescriptorSetLayoutKey::Hasher>
      descriptor_set_layouts_textures_;
  /** Referenced by VulkanPipelineCache. */
  std::unordered_map<PipelineLayoutKey, PipelineLayout, PipelineLayoutKey::Hasher>
      pipeline_layouts_;

  static constexpr u32 kLinkedTypeDescriptorPoolSetCount = 32768;
  static const VkDescriptorPoolSize kDescriptorPoolSizeUniformBuffer;
  static const VkDescriptorPoolSize kDescriptorPoolSizeStorageBuffer;
  static const VkDescriptorPoolSize kDescriptorPoolSizeTextures[2];
  gpu::vulkan::LinkedTypeDescriptorSetAllocator transient_descriptor_allocator_uniform_buffer_;
  gpu::vulkan::LinkedTypeDescriptorSetAllocator transient_descriptor_allocator_storage_buffer_;
  std::deque<UsedSingleTransientDescriptor> single_transient_descriptors_used_;
  std::array<std::vector<VkDescriptorSet>,
             static_cast<size_t>(SingleTransientDescriptorLayout::kCount)>
      single_transient_descriptors_free_;
  /** Pairs of usage frame and set. */
  std::deque<std::pair<u64, VkDescriptorSet>> constants_transient_descriptors_used_;
  std::vector<VkDescriptorSet> constants_transient_descriptors_free_;

  gpu::vulkan::LinkedTypeDescriptorSetAllocator transient_descriptor_allocator_textures_;
  std::deque<UsedTextureTransientDescriptorSet> texture_transient_descriptor_sets_used_;
  std::unordered_map<TextureDescriptorSetLayoutKey, std::vector<VkDescriptorSet>,
                     TextureDescriptorSetLayoutKey::Hasher>
      texture_transient_descriptor_sets_free_;

  std::unique_ptr<VulkanSharedMemory> shared_memory_;

  std::unique_ptr<VulkanPrimitiveProcessor> primitive_processor_;

  std::unique_ptr<VulkanRenderTargetCache> render_target_cache_;

  std::unique_ptr<VulkanPipelineCache> pipeline_cache_;

  std::unique_ptr<VulkanTextureCache> texture_cache_;

  VkDescriptorPool shared_memory_and_edram_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_memory_and_edram_descriptor_set_;

  /**
   * Bytes 0x0 to 0x3FF hold the 256 entry gamma ramp table as B10G10R10X2 data read as R10G10B10X2
   * with swizzle. Bytes 0x400 to 0x9FF hold the 128 entry PWL R16G16 ramp (R base, G delta, low 6
   * bits of each zero, 3 elements per entry). kMaxFramesInFlight pairs of ramps when host visible
   * and uploaded directly, otherwise 1.
   */
  VkDeviceMemory gamma_ramp_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer gamma_ramp_buffer_ = VK_NULL_HANDLE;
  /** kMaxFramesInFlight pairs, only when the gamma ramp buffer is not host visible. */
  VkDeviceMemory gamma_ramp_upload_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer gamma_ramp_upload_buffer_ = VK_NULL_HANDLE;
  VkDeviceSize gamma_ramp_upload_memory_size_;
  u32 gamma_ramp_upload_memory_type_;
  /** Maps gamma_ramp_buffer_memory_ when host visible, else gamma_ramp_upload_buffer_memory_. */
  void* gamma_ramp_upload_mapping_;
  std::array<VkBufferView, size_t{2} * kMaxFramesInFlight> gamma_ramp_buffer_views_{};
  /** UINT32_MAX when outdated. */
  u32 gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
  u32 gamma_ramp_pwl_current_frame_ = UINT32_MAX;

  VkDescriptorSetLayout swap_descriptor_set_layout_sampled_image_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_descriptor_set_layout_combined_image_sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_descriptor_set_layout_storage_image_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_descriptor_set_layout_uniform_texel_buffer_ = VK_NULL_HANDLE;

  VkDescriptorPool swap_descriptor_pool_ = VK_NULL_HANDLE;
  /**
   * Interleaved 256 entry table and PWL texel buffer descriptors, kMaxFramesInFlight pairs when
   * the ramps are host visible and uploaded directly, otherwise 1.
   */
  std::array<VkDescriptorSet, size_t{2} * kMaxFramesInFlight> swap_descriptors_gamma_ramp_;
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_source_;
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_fxaa_source_;
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_destination_storage_;
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_fxaa_destination_storage_;

  VkSampler swap_sampler_linear_clamp_ = VK_NULL_HANDLE;

  VkPipelineLayout swap_apply_gamma_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout swap_apply_gamma_compute_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout swap_fxaa_pipeline_layout_ = VK_NULL_HANDLE;
  /** No stage dependencies at either end, synchronize with explicit barriers. */
  VkRenderPass swap_apply_gamma_render_pass_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_256_entry_table_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_pwl_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_256_entry_table_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_pwl_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_pwl_fxaa_luma_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_256_entry_table_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_pwl_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_256_entry_table_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_pwl_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_256_entry_table_fxaa_luma_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_pwl_fxaa_luma_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_256_entry_table_fxaa_luma_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_compute_pwl_fxaa_luma_rb_swap_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_fxaa_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_fxaa_extreme_pipeline_ = VK_NULL_HANDLE;
  VkPipelineLayout resolve_downscale_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline resolve_downscale_pipeline_ = VK_NULL_HANDLE;
  VkBuffer resolve_downscale_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory resolve_downscale_buffer_memory_ = VK_NULL_HANDLE;
  u32 resolve_downscale_buffer_size_ = 0;

  VkImage swap_fxaa_source_image_ = VK_NULL_HANDLE;
  VkDeviceMemory swap_fxaa_source_image_memory_ = VK_NULL_HANDLE;
  VkImageView swap_fxaa_source_image_view_ = VK_NULL_HANDLE;
  u32 swap_fxaa_source_image_width_ = 0;
  u32 swap_fxaa_source_image_height_ = 0;
  u64 swap_fxaa_source_image_submission_ = 0;
  VkPipelineStageFlags swap_fxaa_source_stage_mask_ = 0;
  VkAccessFlags swap_fxaa_source_access_mask_ = 0;
  VkImageLayout swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

  std::array<SwapFramebuffer, gpu::vulkan::VulkanPresenter::kMaxActiveGuestOutputImageVersions>
      swap_framebuffers_;

  std::vector<VkBufferMemoryBarrier> pending_barriers_buffer_memory_barriers_;
  std::vector<VkImageMemoryBarrier> pending_barriers_image_memory_barriers_;
  struct PendingBarrier {
    VkPipelineStageFlags src_stage_mask = 0;
    VkPipelineStageFlags dst_stage_mask = 0;
    size_t buffer_memory_barriers_offset = 0;
    size_t image_memory_barriers_offset = 0;
  };
  std::vector<PendingBarrier> pending_barriers_;
  PendingBarrier current_pending_barrier_;

  static constexpr VkDeviceSize kScratchBufferSizeIncrement = VkDeviceSize{16} * 1024 * 1024;
  VkDeviceMemory scratch_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer scratch_buffer_ = VK_NULL_HANDLE;
  VkDeviceSize scratch_buffer_size_ = 0;
  VkPipelineStageFlags scratch_buffer_last_stage_mask_ = 0;
  VkAccessFlags scratch_buffer_last_access_mask_ = 0;
  u64 scratch_buffer_last_usage_submission_ = 0;
  bool scratch_buffer_used_ = false;

  static constexpr u32 kMaxOcclusionQueries = 8192;
  VkQueryPool occlusion_query_pool_ = VK_NULL_HANDLE;
  VkBuffer occlusion_query_readback_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory occlusion_query_readback_memory_ = VK_NULL_HANDLE;
  u32 occlusion_query_readback_memory_type_ = UINT32_MAX;
  VkDeviceSize occlusion_query_readback_memory_size_ = 0;
  u8* occlusion_query_readback_mapping_ = nullptr;
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
  std::unordered_map<u64, ReadbackBuffer> readback_buffers_;
  std::unordered_map<u64, ReadbackBuffer> memexport_readback_buffers_;

  /** Binding any pipeline with static state invalidates the dynamic state, even unused values. */
  VkViewport dynamic_viewport_;
  VkRect2D dynamic_scissor_;
  f32 dynamic_depth_bias_constant_factor_;
  f32 dynamic_depth_bias_slope_factor_;
  f32 dynamic_blend_constants_[4];
  /**
   * The stencil values start initialized and equal for front and back for portability subset
   * safety, because updates are conditional to keep the back face values unchanged when stencil
   * is disabled and the primitive type switches between polygonal and non polygonal.
   */
  u32 dynamic_stencil_compare_mask_front_ = UINT8_MAX;
  u32 dynamic_stencil_compare_mask_back_ = UINT8_MAX;
  u32 dynamic_stencil_write_mask_front_ = UINT8_MAX;
  u32 dynamic_stencil_write_mask_back_ = UINT8_MAX;
  u32 dynamic_stencil_reference_front_ = 0;
  u32 dynamic_stencil_reference_back_ = 0;
  bool dynamic_viewport_update_needed_;
  bool dynamic_scissor_update_needed_;
  bool dynamic_depth_bias_update_needed_;
  bool dynamic_blend_constants_update_needed_;
  bool dynamic_stencil_compare_mask_front_update_needed_;
  bool dynamic_stencil_compare_mask_back_update_needed_;
  bool dynamic_stencil_write_mask_front_update_needed_;
  bool dynamic_stencil_write_mask_back_update_needed_;
  bool dynamic_stencil_reference_front_update_needed_;
  bool dynamic_stencil_reference_back_update_needed_;

  std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>> current_samplers_vertex_;
  std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>> current_samplers_pixel_;

  /** VK_NULL_HANDLE with dynamic rendering, where in_render_pass_ is still true. */
  VkRenderPass current_render_pass_;
  const VulkanRenderTargetCache::Framebuffer* current_framebuffer_;
  bool in_render_pass_ = false;

  /** Exactly one of the title and external graphics pipelines is bound, the other is null. */
  VkPipeline current_guest_graphics_pipeline_;
  VkPipeline current_external_graphics_pipeline_;
  VkPipeline current_external_compute_pipeline_;

  const PipelineLayout* current_guest_graphics_pipeline_layout_;
  VkDescriptorBufferInfo
      current_constant_buffer_infos_[SPIRVShaderTranslator::kConstantBufferCount];
  /** Mask of constant buffers with up to date data that current_constant_buffer_infos_ cover. */
  u32 current_constant_buffers_up_to_date_;
  VkDescriptorSet current_graphics_descriptor_sets_[SPIRVShaderTranslator::kDescriptorSetCount];
  /** Mask of sets in current_graphics_descriptor_sets_ that point to up to date data. */
  u32 current_graphics_descriptor_set_values_up_to_date_;
  /**
   * Mask of sets bound to the command buffer. A pipeline layout change keeps only the low bits of
   * unchanged set layouts. May differ from current_graphics_descriptor_set_values_up_to_date_ and
   * may hold bits for nonexistent set layouts, which readers ignore.
   */
  u32 current_graphics_descriptor_sets_bound_up_to_date_;
  static_assert(SPIRVShaderTranslator::kDescriptorSetCount <=
                    sizeof(current_graphics_descriptor_set_values_up_to_date_) * CHAR_BIT,
                "Bit fields storing descriptor set validity must be large enough");
  static_assert(SPIRVShaderTranslator::kDescriptorSetCount <=
                    sizeof(current_graphics_descriptor_sets_bound_up_to_date_) * CHAR_BIT,
                "Bit fields storing descriptor set validity must be large enough");

  u64 current_float_constant_map_vertex_[4];
  u64 current_float_constant_map_pixel_[4];

  SPIRVShaderTranslator::SystemConstants system_constants_;

  std::vector<draw_state::MemExportRange> memexport_ranges_;
};

}  // namespace rex::gpu::vulkan
