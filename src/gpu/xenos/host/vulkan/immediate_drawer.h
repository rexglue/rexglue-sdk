/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include "gpu/xenos/host/immediate_drawer.h"
#include "gpu/xenos/host/vulkan/ui_samplers.h"
#include "gpu/xenos/host/vulkan/upload_buffer_pool.h"

namespace rex {
namespace gpu {
namespace vulkan {

class VulkanImmediateDrawer : public OverlayDrawer {
  struct Key {};

 public:
  explicit VulkanImmediateDrawer(Key, const VulkanDevice* vulkan_device,
                                 const UISamplers* ui_samplers);

  static std::unique_ptr<VulkanImmediateDrawer> Create(const VulkanDevice* vulkan_device,
                                                       const UISamplers* ui_samplers);

  ~VulkanImmediateDrawer();

  [[nodiscard]] Result<std::unique_ptr<gpu::OverlayTexture>> CreateTexture(
      u32 width, u32 height, gpu::OverlayTextureFilter filter, bool is_repeated,
      std::span<const u8> rgba8) override;

  void Begin(gpu::OverlayDrawContext& context, f32 width, f32 height) override;
  void BeginBatch(const gpu::OverlayBatch& batch) override;
  void Draw(const gpu::OverlayDraw& draw) override;
  void EndBatch() override;
  void End() override;

 protected:
  void OnLeavePresenter() override;

 private:
  struct PushConstants {
    struct Vertex {
      f32 coordinate_space_size_inv[2];
    } vertex;
  };

  class VulkanOverlayTexture : public gpu::OverlayTexture {
   public:
    struct Resource {
      VkImage image;
      VkDeviceMemory memory;
      VkImageView image_view;
      u32 descriptor_index;
    };

    VulkanOverlayTexture(u32 width, u32 height)
        : OverlayTexture(width, height), immediate_drawer(nullptr) {}
    ~VulkanOverlayTexture() override;

    /** Null for a blank texture or after the immediate drawer has been destroyed. */
    VulkanImmediateDrawer* immediate_drawer;
    size_t immediate_drawer_index;
    /** Invalid when immediate_drawer is null, as the immediate drawer manages it. */
    Resource resource;
    size_t pending_upload_index;
    u64 last_usage_submission = 0;
  };

  struct TextureDescriptorPool {
    // Using uint64_t for recycled bits.
    static constexpr u32 kDescriptorCount = 64;
    VkDescriptorPool pool;
    VkDescriptorSet sets[kDescriptorCount];
    u32 index;
    u32 unallocated_count;
    u64 recycled_bits;
    TextureDescriptorPool* unallocated_next;
    TextureDescriptorPool* recycled_next;
  };

  bool Initialize();

  bool CreatePipelinesForCurrentRenderPass();

  /**
   * Allocates a combined image sampler in a pool.
   *
   * @return The descriptor index, or UINT32_MAX on failure.
   */
  u32 AllocateTextureDescriptor();
  VkDescriptorSet GetTextureDescriptor(u32 descriptor_index) const;
  void FreeTextureDescriptor(u32 descriptor_index);

  /**
   * Creates a texture resource.
   *
   * If data is null, a (1, 1, 1, 1) image is created to stand in when drawing without a texture.
   *
   * @param width Width in texels.
   * @param height Height in texels.
   * @param filter Sampler filter.
   * @param is_repeated True when the sampler repeats.
   * @param data RGBA8 texel data, or null.
   * @param resource_out Receives the created resource.
   * @param pending_upload_index_out Receives the pending upload index.
   * @return True on success.
   */
  bool CreateTextureResource(u32 width, u32 height, gpu::OverlayTextureFilter filter,
                             bool is_repeated, const u8* data,
                             VulkanOverlayTexture::Resource& resource_out,
                             size_t& pending_upload_index_out);
  void DestroyTextureResource(VulkanOverlayTexture::Resource& resource);
  void OnImmediateTextureDestroyed(VulkanOverlayTexture& texture);

  const VulkanDevice* vulkan_device_;
  const UISamplers* ui_samplers_;

  VkDescriptorSetLayout texture_descriptor_set_layout_;
  std::vector<TextureDescriptorPool*> texture_descriptor_pools_;
  TextureDescriptorPool* texture_descriptor_pool_unallocated_first_ = nullptr;
  TextureDescriptorPool* texture_descriptor_pool_recycled_first_ = nullptr;

  VulkanOverlayTexture::Resource white_texture_ = {};
  std::vector<VulkanOverlayTexture*> textures_;
  struct PendingTextureUpload {
    /** Null for internal resources such as the white texture. */
    VulkanOverlayTexture* texture;
    /** VK_NULL_HANDLE when the image is cleared rather than copied. */
    VkBuffer buffer;
    VkDeviceMemory buffer_memory;
    VkImage image;
    u32 width;
    u32 height;
  };
  std::vector<PendingTextureUpload> texture_uploads_pending_;
  struct SubmittedTextureUploadBuffer {
    VkBuffer buffer;
    VkDeviceMemory buffer_memory;
    u64 submission_index;
  };
  std::deque<SubmittedTextureUploadBuffer> texture_upload_buffers_submitted_;
  std::vector<std::pair<VulkanOverlayTexture::Resource, u64>> textures_deleted_;

  std::unique_ptr<VulkanUploadBufferPool> vertex_buffer_pool_;

  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;

  VkFormat pipeline_framebuffer_format_ = VK_FORMAT_UNDEFINED;
  VkPipeline pipeline_triangle_ = VK_NULL_HANDLE;
  VkPipeline pipeline_line_ = VK_NULL_HANDLE;

  /** Submission index within the current Begin, or within the last one when outside Begin. */
  u64 last_paint_submission_index_ = 0;
  /** Completed submission index as of the latest Begin, to coarsely skip delayed deletion. */
  u64 last_completed_submission_index_ = 0;

  VkRect2D current_scissor_;
  VkPipeline current_pipeline_;
  u32 current_texture_descriptor_index_;
  bool batch_open_ = false;
  bool batch_has_index_buffer_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
