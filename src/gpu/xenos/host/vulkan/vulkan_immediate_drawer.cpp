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

#include <algorithm>
#include <cstring>
#include <format>
#include <iterator>
#include <utility>

#include <rex/assert.h>
#include <rex/logging/log.h>
#include <rex/math.h>
#include <rex/bit.h>
#include "gpu/xenos/host/vulkan/immediate_drawer.h"
#include "gpu/xenos/host/vulkan/presenter.h"
#include "gpu/xenos/host/vulkan/objects.h"

namespace rex {
namespace gpu {
namespace vulkan {

namespace shaders {
#include "../shaders/vulkan_spirv/immediate_ps.h"
#include "../shaders/vulkan_spirv/immediate_vs.h"
}  // namespace shaders

std::unique_ptr<VulkanImmediateDrawer> VulkanImmediateDrawer::Create(
    const VulkanDevice* const vulkan_device, const UISamplerSet* const ui_samplers) {
  REX_ASSERT(vulkan_device != nullptr);
  REX_ASSERT(ui_samplers != nullptr);

  auto immediate_drawer =
      std::make_unique<VulkanImmediateDrawer>(Key{}, vulkan_device, ui_samplers);
  if (!immediate_drawer->Initialize()) {
    return nullptr;
  }
  return immediate_drawer;
}

VulkanImmediateDrawer::VulkanOverlayTexture::~VulkanOverlayTexture() {
  if (immediate_drawer) {
    log::NoThrow(log::gpu, [this] { immediate_drawer->OnImmediateTextureDestroyed(*this); });
  }
}

VulkanImmediateDrawer::~VulkanImmediateDrawer() {
  auto vulkan_presenter = static_cast<VulkanPresenter*>(GetPresenter());
  if (vulkan_presenter) {
    vulkan_presenter->AwaitUISubmissionCompletionFromUIThread(last_paint_submission_index_);
  }

  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();

  objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, pipeline_line_);
  objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, pipeline_triangle_);

  objects::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, pipeline_layout_);

  for (auto& deleted_texture : textures_deleted_) {
    DestroyTextureResource(deleted_texture.first);
  }
  for (SubmittedTextureUploadBuffer& submitted_texture_upload_buffer :
       texture_upload_buffers_submitted_) {
    dfn.vkDestroyBuffer(device, submitted_texture_upload_buffer.buffer, nullptr);
    dfn.vkFreeMemory(device, submitted_texture_upload_buffer.buffer_memory, nullptr);
  }
  for (PendingTextureUpload& pending_texture_upload : texture_uploads_pending_) {
    dfn.vkDestroyBuffer(device, pending_texture_upload.buffer, nullptr);
    dfn.vkFreeMemory(device, pending_texture_upload.buffer_memory, nullptr);
  }
  for (VulkanOverlayTexture* texture : textures_) {
    if (texture->immediate_drawer != this) {
      continue;
    }
    texture->immediate_drawer = nullptr;
    DestroyTextureResource(texture->resource);
  }
  if (white_texture_.image != VK_NULL_HANDLE) {
    DestroyTextureResource(white_texture_);
  }

  for (TextureDescriptorPool* pool : texture_descriptor_pools_) {
    dfn.vkDestroyDescriptorPool(device, pool->pool, nullptr);
    delete pool;
  }
  objects::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                texture_descriptor_set_layout_);
}

VulkanImmediateDrawer::VulkanImmediateDrawer(Key, const VulkanDevice* const vulkan_device,
                                             const UISamplerSet* const ui_samplers)
    : vulkan_device_(vulkan_device), ui_samplers_(ui_samplers) {
  REX_ASSERT(vulkan_device != nullptr);
  REX_ASSERT(ui_samplers != nullptr);
}

bool VulkanImmediateDrawer::Initialize() {
  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();

  VkDescriptorSetLayoutBinding texture_descriptor_set_layout_binding;
  texture_descriptor_set_layout_binding.binding = 0;
  texture_descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  texture_descriptor_set_layout_binding.descriptorCount = 1;
  texture_descriptor_set_layout_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  texture_descriptor_set_layout_binding.pImmutableSamplers = nullptr;
  VkDescriptorSetLayoutCreateInfo texture_descriptor_set_layout_create_info;
  texture_descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  texture_descriptor_set_layout_create_info.pNext = nullptr;
  texture_descriptor_set_layout_create_info.flags = 0;
  texture_descriptor_set_layout_create_info.bindingCount = 1;
  texture_descriptor_set_layout_create_info.pBindings = &texture_descriptor_set_layout_binding;
  if (dfn.vkCreateDescriptorSetLayout(device, &texture_descriptor_set_layout_create_info, nullptr,
                                      &texture_descriptor_set_layout_) != VK_SUCCESS) {
    REXUI_FAIL(false,
               "VulkanImmediateDrawer: Failed to create the combined image sampler "
               "descriptor set layout");
  }

  size_t white_texture_pending_upload_index;
  if (!CreateTextureResource(1, 1, gpu::OverlayTextureFilter::kNearest, false, nullptr,
                             white_texture_, white_texture_pending_upload_index)) {
    REXUI_FAIL(false, "VulkanImmediateDrawer: Failed to create a white texture");
  }

  vertex_buffer_pool_ = std::make_unique<VulkanUploadBufferPool>(
      vulkan_device_, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

  VkPushConstantRange push_constant_ranges[1];
  push_constant_ranges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  push_constant_ranges[0].offset = offsetof(PushConstants, vertex);
  push_constant_ranges[0].size = sizeof(PushConstants::Vertex);
  VkPipelineLayoutCreateInfo pipeline_layout_create_info;
  pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_create_info.pNext = nullptr;
  pipeline_layout_create_info.flags = 0;
  pipeline_layout_create_info.setLayoutCount = 1;
  pipeline_layout_create_info.pSetLayouts = &texture_descriptor_set_layout_;
  pipeline_layout_create_info.pushConstantRangeCount =
      static_cast<u32>(rex::CountOf(push_constant_ranges));
  pipeline_layout_create_info.pPushConstantRanges = push_constant_ranges;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_create_info, nullptr,
                                 &pipeline_layout_) != VK_SUCCESS) {
    REXUI_FAIL(false, "VulkanImmediateDrawer: Failed to create the pipeline layout");
  }

  batch_open_ = false;

  return true;
}

Result<std::unique_ptr<gpu::OverlayTexture>> VulkanImmediateDrawer::CreateTexture(
    u32 width, u32 height, gpu::OverlayTextureFilter filter, bool is_repeated,
    std::span<const u8> rgba8) {
  const u8* data = rgba8.data();
  REX_ASSERT(data != nullptr);
  auto texture = std::make_unique<VulkanOverlayTexture>(width, height);
  size_t pending_upload_index;
  if (!CreateTextureResource(width, height, filter, is_repeated, data, texture->resource,
                             pending_upload_index)) {
    return Err<std::unique_ptr<gpu::OverlayTexture>>(
        ErrorCategory::kRuntime, std::format("Failed to create a {}x{} texture", width, height));
  }
  texture->immediate_drawer = this;
  texture->immediate_drawer_index = textures_.size();
  textures_.push_back(texture.get());
  texture->pending_upload_index = pending_upload_index;
  texture_uploads_pending_[texture->pending_upload_index].texture = texture.get();
  return std::unique_ptr<gpu::OverlayTexture>(std::move(texture));
}

void VulkanImmediateDrawer::Begin(gpu::OverlayDrawContext& context, f32 width, f32 height) {
  OverlayDrawer::Begin(context, width, height);

  REX_ASSERT(!batch_open_);

  const VulkanOverlayDrawContext& vulkan_overlay_draw_context =
      static_cast<const VulkanOverlayDrawContext&>(context);

  last_paint_submission_index_ = vulkan_overlay_draw_context.GetSubmissionIndexCurrent();
  last_completed_submission_index_ = vulkan_overlay_draw_context.GetSubmissionIndexCompleted();

  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();

  for (auto it = textures_deleted_.begin(); it != textures_deleted_.end();) {
    if (it->second > last_completed_submission_index_) {
      ++it;
      continue;
    }
    DestroyTextureResource(it->first);
    if (std::next(it) != textures_deleted_.end()) {
      *it = textures_deleted_.back();
    }
    textures_deleted_.pop_back();
  }

  auto erase_texture_uploads_end = texture_upload_buffers_submitted_.begin();
  while (erase_texture_uploads_end != texture_upload_buffers_submitted_.end()) {
    if (erase_texture_uploads_end->submission_index > last_completed_submission_index_) {
      break;
    }
    dfn.vkDestroyBuffer(device, erase_texture_uploads_end->buffer, nullptr);
    dfn.vkFreeMemory(device, erase_texture_uploads_end->buffer_memory, nullptr);
    ++erase_texture_uploads_end;
  }
  texture_upload_buffers_submitted_.erase(texture_upload_buffers_submitted_.begin(),
                                          erase_texture_uploads_end);

  vertex_buffer_pool_->Reclaim(last_completed_submission_index_);

  if (!CreatePipelinesForCurrentRenderPass()) {
    return;
  }

  VkCommandBuffer draw_command_buffer = vulkan_overlay_draw_context.GetDrawCommandBuffer();

  VkViewport viewport;
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = static_cast<f32>(vulkan_overlay_draw_context.GetRenderTargetWidth());
  viewport.height = static_cast<f32>(vulkan_overlay_draw_context.GetRenderTargetHeight());
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  dfn.vkCmdSetViewport(draw_command_buffer, 0, 1, &viewport);
  PushConstants::Vertex push_constants_vertex;
  push_constants_vertex.coordinate_space_size_inv[0] = 1.0f / GetCoordinateSpaceWidth();
  push_constants_vertex.coordinate_space_size_inv[1] = 1.0f / GetCoordinateSpaceHeight();
  dfn.vkCmdPushConstants(draw_command_buffer, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                         offsetof(PushConstants, vertex), sizeof(PushConstants::Vertex),
                         &push_constants_vertex);
  current_scissor_.offset.x = 0;
  current_scissor_.offset.y = 0;
  current_scissor_.extent.width = 0;
  current_scissor_.extent.height = 0;

  current_pipeline_ = VK_NULL_HANDLE;
  current_texture_descriptor_index_ = UINT32_MAX;
}

void VulkanImmediateDrawer::BeginBatch(const gpu::OverlayBatch& batch) {
  REX_ASSERT(!batch_open_);

  const VulkanOverlayDrawContext& vulkan_overlay_draw_context =
      *static_cast<const VulkanOverlayDrawContext*>(GetOverlayDrawContext());
  VkCommandBuffer draw_command_buffer = vulkan_overlay_draw_context.GetDrawCommandBuffer();

  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();

  size_t vertex_buffer_size = sizeof(gpu::OverlayVertex) * batch.vertices.size();
  VkBuffer vertex_buffer;
  VkDeviceSize vertex_buffer_offset;
  void* vertex_buffer_mapping =
      vertex_buffer_pool_->Request(last_paint_submission_index_, vertex_buffer_size, sizeof(f32),
                                   vertex_buffer, vertex_buffer_offset);
  if (!vertex_buffer_mapping) {
    REXUI_ERROR("VulkanImmediateDrawer: Failed to get a buffer for {} vertices",
                batch.vertices.size());
    return;
  }
  std::memcpy(vertex_buffer_mapping, batch.vertices.data(), vertex_buffer_size);
  dfn.vkCmdBindVertexBuffers(draw_command_buffer, 0, 1, &vertex_buffer, &vertex_buffer_offset);

  batch_has_index_buffer_ = !batch.indices.empty();
  if (batch_has_index_buffer_) {
    size_t index_buffer_size = sizeof(u16) * batch.indices.size();
    VkBuffer index_buffer;
    VkDeviceSize index_buffer_offset;
    void* index_buffer_mapping =
        vertex_buffer_pool_->Request(last_paint_submission_index_, index_buffer_size, sizeof(u16),
                                     index_buffer, index_buffer_offset);
    if (!index_buffer_mapping) {
      REXUI_ERROR("VulkanImmediateDrawer: Failed to get a buffer for {} indices",
                  batch.indices.size());
      return;
    }
    std::memcpy(index_buffer_mapping, batch.indices.data(), index_buffer_size);
    dfn.vkCmdBindIndexBuffer(draw_command_buffer, index_buffer, index_buffer_offset,
                             VK_INDEX_TYPE_UINT16);
  }

  batch_open_ = true;
}

void VulkanImmediateDrawer::Draw(const gpu::OverlayDraw& draw) {
  if (!batch_open_) {
    return;
  }

  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VulkanOverlayDrawContext& vulkan_overlay_draw_context =
      *static_cast<const VulkanOverlayDrawContext*>(GetOverlayDrawContext());
  VkCommandBuffer draw_command_buffer = vulkan_overlay_draw_context.GetDrawCommandBuffer();

  VkPipeline pipeline;
  switch (draw.primitive) {
    case gpu::OverlayPrimitive::kLines:
      pipeline = pipeline_line_;
      break;
    case gpu::OverlayPrimitive::kTriangles:
      pipeline = pipeline_triangle_;
      break;
    default:
      REX_UNHANDLED_CASE(draw.primitive);
      return;
  }
  if (pipeline == VK_NULL_HANDLE) {
    return;
  }

  VkRect2D scissor;
  u32 scissor_left, scissor_top;
  if (!ScissorToRenderTarget(draw, scissor_left, scissor_top, scissor.extent.width,
                             scissor.extent.height)) {
    // Zero area, the default current_scissor_ value, means nothing is visible.
    return;
  }
  scissor.offset.x = static_cast<i32>(scissor_left);
  scissor.offset.y = static_cast<i32>(scissor_top);
  if (current_scissor_.offset.x != scissor.offset.x ||
      current_scissor_.offset.y != scissor.offset.y ||
      current_scissor_.extent.width != scissor.extent.width ||
      current_scissor_.extent.height != scissor.extent.height) {
    current_scissor_ = scissor;
    dfn.vkCmdSetScissor(draw_command_buffer, 0, 1, &scissor);
  }

  if (current_pipeline_ != pipeline) {
    current_pipeline_ = pipeline;
    dfn.vkCmdBindPipeline(draw_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  }

  u32 texture_descriptor_index;
  VulkanOverlayTexture* texture = static_cast<VulkanOverlayTexture*>(draw.texture);
  if (texture && texture->immediate_drawer == this) {
    texture_descriptor_index = texture->resource.descriptor_index;
    texture->last_usage_submission = last_paint_submission_index_;
  } else {
    texture_descriptor_index = white_texture_.descriptor_index;
  }
  if (current_texture_descriptor_index_ != texture_descriptor_index) {
    current_texture_descriptor_index_ = texture_descriptor_index;
    VkDescriptorSet texture_descriptor_set = GetTextureDescriptor(texture_descriptor_index);
    dfn.vkCmdBindDescriptorSets(draw_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout_, 0, 1, &texture_descriptor_set, 0, nullptr);
  }

  if (batch_has_index_buffer_) {
    dfn.vkCmdDrawIndexed(draw_command_buffer, draw.count, 1, draw.index_offset, draw.base_vertex,
                         0);
  } else {
    dfn.vkCmdDraw(draw_command_buffer, draw.count, 1, draw.base_vertex, 0);
  }
}

void VulkanImmediateDrawer::EndBatch() {
  batch_open_ = false;
}

void VulkanImmediateDrawer::End() {
  REX_ASSERT(!batch_open_);

  if (!texture_uploads_pending_.empty()) {
    VulkanPresenter& vulkan_presenter = *static_cast<VulkanPresenter*>(GetPresenter());
    VkCommandBuffer setup_command_buffer =
        vulkan_presenter.AcquireUISetupCommandBufferFromUIThread();
    if (setup_command_buffer != VK_NULL_HANDLE) {
      size_t texture_uploads_pending_count = texture_uploads_pending_.size();
      const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();

      std::vector<VkImageMemoryBarrier> image_memory_barriers;
      image_memory_barriers.reserve(texture_uploads_pending_count);
      VkImageMemoryBarrier image_memory_barrier;
      image_memory_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      image_memory_barrier.pNext = nullptr;
      image_memory_barrier.srcAccessMask = 0;
      image_memory_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      image_memory_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      image_memory_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      image_memory_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      image_memory_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      image_memory_barrier.subresourceRange = objects::BuildSubresourceRange();
      for (const PendingTextureUpload& pending_texture_upload : texture_uploads_pending_) {
        image_memory_barriers.emplace_back(image_memory_barrier).image =
            pending_texture_upload.image;
      }
      dfn.vkCmdPipelineBarrier(setup_command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                               static_cast<u32>(image_memory_barriers.size()),
                               image_memory_barriers.data());

      for (size_t i = 0; i < texture_uploads_pending_count; ++i) {
        const PendingTextureUpload& pending_texture_upload = texture_uploads_pending_[i];
        if (pending_texture_upload.buffer != VK_NULL_HANDLE) {
          VkBufferImageCopy copy_region;
          copy_region.bufferOffset = 0;
          copy_region.bufferRowLength = pending_texture_upload.width;
          copy_region.bufferImageHeight = pending_texture_upload.height;
          copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          copy_region.imageSubresource.mipLevel = 0;
          copy_region.imageSubresource.baseArrayLayer = 0;
          copy_region.imageSubresource.layerCount = 1;
          copy_region.imageOffset.x = 0;
          copy_region.imageOffset.y = 0;
          copy_region.imageOffset.z = 0;
          copy_region.imageExtent.width = pending_texture_upload.width;
          copy_region.imageExtent.height = pending_texture_upload.height;
          copy_region.imageExtent.depth = 1;
          dfn.vkCmdCopyBufferToImage(setup_command_buffer, pending_texture_upload.buffer,
                                     pending_texture_upload.image,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

          SubmittedTextureUploadBuffer& submitted_texture_upload_buffer =
              texture_upload_buffers_submitted_.emplace_back();
          submitted_texture_upload_buffer.buffer = pending_texture_upload.buffer;
          submitted_texture_upload_buffer.buffer_memory = pending_texture_upload.buffer_memory;
          submitted_texture_upload_buffer.submission_index = last_paint_submission_index_;
        } else {
          VkClearColorValue white_clear_value;
          white_clear_value.float32[0] = 1.0f;
          white_clear_value.float32[1] = 1.0f;
          white_clear_value.float32[2] = 1.0f;
          white_clear_value.float32[3] = 1.0f;
          dfn.vkCmdClearColorImage(setup_command_buffer, pending_texture_upload.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white_clear_value, 1,
                                   &image_memory_barrier.subresourceRange);
        }

        VkImageMemoryBarrier& image_memory_barrier_current = image_memory_barriers[i];
        image_memory_barrier_current.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        image_memory_barrier_current.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        image_memory_barrier_current.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        image_memory_barrier_current.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        if (pending_texture_upload.texture) {
          pending_texture_upload.texture->last_usage_submission = last_paint_submission_index_;
          pending_texture_upload.texture->pending_upload_index = SIZE_MAX;
        }
      }
      dfn.vkCmdPipelineBarrier(setup_command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                               static_cast<u32>(image_memory_barriers.size()),
                               image_memory_barriers.data());

      texture_uploads_pending_.clear();
    }
  }

  vertex_buffer_pool_->FlushWrites();

  OverlayDrawer::End();
}

void VulkanImmediateDrawer::OnLeavePresenter() {
  VulkanPresenter& vulkan_presenter = *static_cast<VulkanPresenter*>(GetPresenter());
  vulkan_presenter.AwaitUISubmissionCompletionFromUIThread(last_paint_submission_index_);

  for (VulkanOverlayTexture* texture : textures_) {
    texture->last_usage_submission = 0;
  }

  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();

  for (SubmittedTextureUploadBuffer& submitted_texture_upload_buffer :
       texture_upload_buffers_submitted_) {
    dfn.vkDestroyBuffer(device, submitted_texture_upload_buffer.buffer, nullptr);
    dfn.vkFreeMemory(device, submitted_texture_upload_buffer.buffer_memory, nullptr);
  }

  vertex_buffer_pool_->ResetSubmissionTimeline();

  last_paint_submission_index_ = 0;
  last_completed_submission_index_ = 0;
}

bool VulkanImmediateDrawer::CreatePipelinesForCurrentRenderPass() {
  const VulkanOverlayDrawContext& vulkan_overlay_draw_context =
      *static_cast<const VulkanOverlayDrawContext*>(GetOverlayDrawContext());
  VkFormat render_pass_format = vulkan_overlay_draw_context.GetRenderPassFormat();

  if (render_pass_format == pipeline_framebuffer_format_) {
    // True when the pipelines exist. A failed creation is not retried.
    return pipeline_triangle_ != VK_NULL_HANDLE && pipeline_line_ != VK_NULL_HANDLE;
  }

  if (last_paint_submission_index_ && pipeline_triangle_ != VK_NULL_HANDLE &&
      pipeline_line_ != VK_NULL_HANDLE) {
    VulkanPresenter& vulkan_presenter = *static_cast<VulkanPresenter*>(GetPresenter());
    vulkan_presenter.AwaitUISubmissionCompletionFromUIThread(last_paint_submission_index_);
  }

  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();

  objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, pipeline_line_);
  objects::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, pipeline_triangle_);
  pipeline_framebuffer_format_ = render_pass_format;

  VkPipelineShaderStageCreateInfo stages[2] = {
      objects::BuildShaderStage(VK_SHADER_STAGE_VERTEX_BIT, VK_NULL_HANDLE),
      objects::BuildShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, VK_NULL_HANDLE)};
  stages[0].module = objects::CreateShaderModule(vulkan_device_, shaders::kImmediateVS,
                                                 sizeof(shaders::kImmediateVS));
  if (stages[0].module == VK_NULL_HANDLE) {
    REXUI_FAIL(false, "VulkanImmediateDrawer: Failed to create the vertex shader module");
  }
  stages[1].module = objects::CreateShaderModule(vulkan_device_, shaders::kImmediatePS,
                                                 sizeof(shaders::kImmediatePS));
  if (stages[1].module == VK_NULL_HANDLE) {
    REXUI_ERROR("VulkanImmediateDrawer: Failed to create the fragment shader module");
    dfn.vkDestroyShaderModule(device, stages[0].module, nullptr);
    return false;
  }

  VkVertexInputBindingDescription vertex_input_binding;
  vertex_input_binding.binding = 0;
  vertex_input_binding.stride = sizeof(gpu::OverlayVertex);
  vertex_input_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription vertex_input_attributes[3];
  vertex_input_attributes[0].location = 0;
  vertex_input_attributes[0].binding = 0;
  vertex_input_attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
  vertex_input_attributes[0].offset = offsetof(gpu::OverlayVertex, x);
  vertex_input_attributes[1].location = 1;
  vertex_input_attributes[1].binding = 0;
  vertex_input_attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
  vertex_input_attributes[1].offset = offsetof(gpu::OverlayVertex, u);
  vertex_input_attributes[2].location = 2;
  vertex_input_attributes[2].binding = 0;
  vertex_input_attributes[2].format = VK_FORMAT_R8G8B8A8_UNORM;
  vertex_input_attributes[2].offset = offsetof(gpu::OverlayVertex, color);
  VkPipelineVertexInputStateCreateInfo vertex_input_state;
  vertex_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertex_input_state.pNext = nullptr;
  vertex_input_state.flags = 0;
  vertex_input_state.vertexBindingDescriptionCount = 1;
  vertex_input_state.pVertexBindingDescriptions = &vertex_input_binding;
  vertex_input_state.vertexAttributeDescriptionCount =
      static_cast<u32>(rex::CountOf(vertex_input_attributes));
  vertex_input_state.pVertexAttributeDescriptions = vertex_input_attributes;

  VkPipelineInputAssemblyStateCreateInfo input_assembly_state;
  input_assembly_state.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly_state.pNext = nullptr;
  input_assembly_state.flags = 0;
  input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  input_assembly_state.primitiveRestartEnable = VK_FALSE;

  VkPipelineViewportStateCreateInfo viewport_state;
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.pNext = nullptr;
  viewport_state.flags = 0;
  viewport_state.viewportCount = 1;
  viewport_state.pViewports = nullptr;
  viewport_state.scissorCount = 1;
  viewport_state.pScissors = nullptr;

  VkPipelineRasterizationStateCreateInfo rasterization_state = {};
  rasterization_state.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization_state.cullMode = VK_CULL_MODE_NONE;
  rasterization_state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterization_state.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample_state =
      objects::BuildMultisampleState(VK_SAMPLE_COUNT_1_BIT);

  VkPipelineColorBlendAttachmentState color_blend_attachment_state;
  color_blend_attachment_state.blendEnable = VK_TRUE;
  color_blend_attachment_state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  color_blend_attachment_state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  color_blend_attachment_state.colorBlendOp = VK_BLEND_OP_ADD;
  color_blend_attachment_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  color_blend_attachment_state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  color_blend_attachment_state.alphaBlendOp = VK_BLEND_OP_ADD;
  color_blend_attachment_state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                                VK_COLOR_COMPONENT_G_BIT |
                                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo color_blend_state = {};
  color_blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blend_state.attachmentCount = 1;
  color_blend_state.pAttachments = &color_blend_attachment_state;

  static const VkDynamicState kDynamicStates[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
  };
  VkPipelineDynamicStateCreateInfo dynamic_state;
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.pNext = nullptr;
  dynamic_state.flags = 0;
  dynamic_state.dynamicStateCount = static_cast<u32>(rex::CountOf(kDynamicStates));
  dynamic_state.pDynamicStates = kDynamicStates;

  VkGraphicsPipelineCreateInfo pipeline_create_info;
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_create_info.pNext = nullptr;
  pipeline_create_info.flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
  pipeline_create_info.stageCount = static_cast<u32>(rex::CountOf(stages));
  pipeline_create_info.pStages = stages;
  pipeline_create_info.pVertexInputState = &vertex_input_state;
  pipeline_create_info.pInputAssemblyState = &input_assembly_state;
  pipeline_create_info.pTessellationState = nullptr;
  pipeline_create_info.pViewportState = &viewport_state;
  pipeline_create_info.pRasterizationState = &rasterization_state;
  pipeline_create_info.pMultisampleState = &multisample_state;
  pipeline_create_info.pDepthStencilState = nullptr;
  pipeline_create_info.pColorBlendState = &color_blend_state;
  pipeline_create_info.pDynamicState = &dynamic_state;
  pipeline_create_info.layout = pipeline_layout_;
  pipeline_create_info.renderPass = vulkan_overlay_draw_context.GetRenderPass();
  pipeline_create_info.subpass = 0;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;
  if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr,
                                    &pipeline_triangle_) != VK_SUCCESS) {
    REXUI_ERROR("VulkanImmediateDrawer: Failed to create the triangle list pipeline");
    dfn.vkDestroyShaderModule(device, stages[1].module, nullptr);
    dfn.vkDestroyShaderModule(device, stages[0].module, nullptr);
    return false;
  }

  input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
  pipeline_create_info.flags =
      (pipeline_create_info.flags & ~VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT) |
      VK_PIPELINE_CREATE_DERIVATIVE_BIT;
  pipeline_create_info.basePipelineHandle = pipeline_triangle_;
  VkResult pipeline_line_create_result = dfn.vkCreateGraphicsPipelines(
      device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr, &pipeline_line_);
  dfn.vkDestroyShaderModule(device, stages[1].module, nullptr);
  dfn.vkDestroyShaderModule(device, stages[0].module, nullptr);
  if (pipeline_line_create_result != VK_SUCCESS) {
    REXUI_ERROR("VulkanImmediateDrawer: Failed to create the line list pipeline");
    dfn.vkDestroyPipeline(device, pipeline_triangle_, nullptr);
    pipeline_triangle_ = VK_NULL_HANDLE;
    return false;
  }

  return true;
}

u32 VulkanImmediateDrawer::AllocateTextureDescriptor() {
  if (texture_descriptor_pool_recycled_first_) {
    TextureDescriptorPool* pool = texture_descriptor_pool_recycled_first_;
    REX_ASSERT(pool->recycled_bits != 0);
    u32 local_index;
    rex::bit_scan_forward(pool->recycled_bits, &local_index);
    pool->recycled_bits &= ~(static_cast<u64>(1) << local_index);
    if (!pool->recycled_bits) {
      texture_descriptor_pool_recycled_first_ = pool->recycled_next;
    }
    return (pool->index << 6) | local_index;
  }

  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();

  VkDescriptorSetAllocateInfo allocate_info;
  allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate_info.pNext = nullptr;
  allocate_info.descriptorSetCount = 1;
  allocate_info.pSetLayouts = &texture_descriptor_set_layout_;

  while (texture_descriptor_pool_unallocated_first_) {
    TextureDescriptorPool* pool = texture_descriptor_pool_unallocated_first_;
    REX_ASSERT(pool->unallocated_count != 0);
    allocate_info.descriptorPool = pool->pool;
    u32 local_index = TextureDescriptorPool::kDescriptorCount - pool->unallocated_count;
    VkResult allocate_result =
        dfn.vkAllocateDescriptorSets(device, &allocate_info, &pool->sets[local_index]);
    if (allocate_result == VK_SUCCESS) {
      --pool->unallocated_count;
    } else {
      // Never retry a pool that failed to allocate.
      pool->unallocated_count = 0;
    }
    if (!pool->unallocated_count) {
      texture_descriptor_pool_unallocated_first_ = pool->unallocated_next;
    }
    if (allocate_result == VK_SUCCESS) {
      return (pool->index << 6) | local_index;
    }
  }

  VkDescriptorPoolSize descriptor_pool_size;
  descriptor_pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  descriptor_pool_size.descriptorCount = TextureDescriptorPool::kDescriptorCount;
  VkDescriptorPoolCreateInfo descriptor_pool_create_info;
  descriptor_pool_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  descriptor_pool_create_info.pNext = nullptr;
  descriptor_pool_create_info.flags = 0;
  descriptor_pool_create_info.maxSets = TextureDescriptorPool::kDescriptorCount;
  descriptor_pool_create_info.poolSizeCount = 1;
  descriptor_pool_create_info.pPoolSizes = &descriptor_pool_size;
  VkDescriptorPool descriptor_pool;
  if (dfn.vkCreateDescriptorPool(device, &descriptor_pool_create_info, nullptr, &descriptor_pool) !=
      VK_SUCCESS) {
    REXUI_FAIL(UINT32_MAX,
               "VulkanImmediateDrawer: Failed to create a combined image sampler "
               "descriptor pool with {} descriptors",
               TextureDescriptorPool::kDescriptorCount);
  }
  allocate_info.descriptorPool = descriptor_pool;
  VkDescriptorSet descriptor_set;
  if (dfn.vkAllocateDescriptorSets(device, &allocate_info, &descriptor_set) != VK_SUCCESS) {
    REXUI_ERROR(
        "VulkanImmediateDrawer: Failed to allocate a combined image sampler "
        "descriptor");
    dfn.vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
    return UINT32_MAX;
  }
  TextureDescriptorPool* new_pool = new TextureDescriptorPool;
  new_pool->pool = descriptor_pool;
  new_pool->sets[0] = descriptor_set;
  u32 new_pool_index = static_cast<u32>(texture_descriptor_pools_.size());
  new_pool->index = new_pool_index;
  new_pool->unallocated_count = TextureDescriptorPool::kDescriptorCount - 1;
  new_pool->recycled_bits = 0;
  new_pool->unallocated_next = texture_descriptor_pool_unallocated_first_;
  texture_descriptor_pool_unallocated_first_ = new_pool;
  new_pool->recycled_next = nullptr;
  texture_descriptor_pools_.push_back(new_pool);
  return new_pool_index << 6;
}

VkDescriptorSet VulkanImmediateDrawer::GetTextureDescriptor(u32 descriptor_index) const {
  u32 pool_index = descriptor_index >> 6;
  REX_ASSERT(pool_index < texture_descriptor_pools_.size());
  const TextureDescriptorPool* pool = texture_descriptor_pools_[pool_index];
  u32 allocation_index = descriptor_index & 63;
  REX_ASSERT(allocation_index < TextureDescriptorPool::kDescriptorCount - pool->unallocated_count);
  return pool->sets[allocation_index];
}

void VulkanImmediateDrawer::FreeTextureDescriptor(u32 descriptor_index) {
  u32 pool_index = descriptor_index >> 6;
  REX_ASSERT(pool_index < texture_descriptor_pools_.size());
  TextureDescriptorPool* pool = texture_descriptor_pools_[pool_index];
  u32 allocation_index = descriptor_index & 63;
  REX_ASSERT(allocation_index < TextureDescriptorPool::kDescriptorCount - pool->unallocated_count);
  REX_ASSERT((pool->recycled_bits & (static_cast<u64>(1) << allocation_index)) == 0);
  if (!pool->recycled_bits) {
    pool->recycled_next = texture_descriptor_pool_recycled_first_;
    texture_descriptor_pool_recycled_first_ = pool;
  }
  pool->recycled_bits |= static_cast<u64>(1) << allocation_index;
}

bool VulkanImmediateDrawer::CreateTextureResource(u32 width, u32 height,
                                                  gpu::OverlayTextureFilter filter,
                                                  bool is_repeated, const u8* data,
                                                  VulkanOverlayTexture::Resource& resource_out,
                                                  size_t& pending_upload_index_out) {
  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();

  VkImageCreateInfo image_create_info;
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.pNext = nullptr;
  image_create_info.flags = 0;
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_create_info.extent.width = width;
  image_create_info.extent.height = height;
  image_create_info.extent.depth = 1;
  image_create_info.mipLevels = 1;
  image_create_info.arrayLayers = 1;
  image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_create_info.queueFamilyIndexCount = 0;
  image_create_info.pQueueFamilyIndices = nullptr;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage image;
  VkDeviceMemory image_memory;
  if (!objects::CreateDedicatedAllocationImage(vulkan_device_, image_create_info,
                                               objects::MemoryPurpose::kDeviceLocal, image,
                                               image_memory)) {
    REXUI_FAIL(false,
               "VulkanImmediateDrawer: Failed to create an image with dedicated "
               "memory for a {}x{} texture",
               width, height);
  }

  VkImageViewCreateInfo image_view_create_info;
  image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  image_view_create_info.pNext = nullptr;
  image_view_create_info.flags = 0;
  image_view_create_info.image = image;
  image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  image_view_create_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  VkComponentSwizzle swizzle = (data || !vulkan_device_->GetProperties().imageViewFormatSwizzle)
                                   ? VK_COMPONENT_SWIZZLE_IDENTITY
                                   : VK_COMPONENT_SWIZZLE_ONE;
  image_view_create_info.components.r = swizzle;
  image_view_create_info.components.g = swizzle;
  image_view_create_info.components.b = swizzle;
  image_view_create_info.components.a = swizzle;
  image_view_create_info.subresourceRange = objects::BuildSubresourceRange();
  VkImageView image_view;
  if (dfn.vkCreateImageView(device, &image_view_create_info, nullptr, &image_view) != VK_SUCCESS) {
    REXUI_ERROR(
        "VulkanImmediateDrawer: Failed to create an image view for a {}x{} "
        "image",
        width, height);
    dfn.vkDestroyImage(device, image, nullptr);
    dfn.vkFreeMemory(device, image_memory, nullptr);
    return false;
  }

  u32 descriptor_index = AllocateTextureDescriptor();
  if (descriptor_index == UINT32_MAX) {
    REXUI_ERROR("VulkanImmediateDrawer: Failed to allocate a descriptor for an image");
    dfn.vkDestroyImageView(device, image_view, nullptr);
    dfn.vkDestroyImage(device, image, nullptr);
    dfn.vkFreeMemory(device, image_memory, nullptr);
    return false;
  }
  VkDescriptorImageInfo descriptor_image_info;
  UISamplerSet::SamplerIndex ui_sampler_index;
  if (filter == gpu::OverlayTextureFilter::kLinear) {
    ui_sampler_index = is_repeated ? UISamplerSet::kSamplerIndexLinearRepeat
                                   : UISamplerSet::kSamplerIndexLinearClampToEdge;
  } else {
    ui_sampler_index = is_repeated ? UISamplerSet::kSamplerIndexNearestRepeat
                                   : UISamplerSet::kSamplerIndexNearestClampToEdge;
  }
  descriptor_image_info.sampler = ui_samplers_->GetSamplers()[ui_sampler_index];
  descriptor_image_info.imageView = image_view;
  descriptor_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet descriptor_write;
  descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptor_write.pNext = nullptr;
  descriptor_write.dstSet = GetTextureDescriptor(descriptor_index);
  descriptor_write.dstBinding = 0;
  descriptor_write.dstArrayElement = 0;
  descriptor_write.descriptorCount = 1;
  descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  descriptor_write.pImageInfo = &descriptor_image_info;
  descriptor_write.pBufferInfo = nullptr;
  descriptor_write.pTexelBufferView = nullptr;
  dfn.vkUpdateDescriptorSets(device, 1, &descriptor_write, 0, nullptr);

  VkBuffer upload_buffer = VK_NULL_HANDLE;
  VkDeviceMemory upload_buffer_memory = VK_NULL_HANDLE;
  if (data) {
    size_t data_size = sizeof(u32) * width * height;
    u32 upload_buffer_memory_type;
    if (!objects::CreateDedicatedAllocationBuffer(
            vulkan_device_, static_cast<VkDeviceSize>(data_size), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            objects::MemoryPurpose::kUpload, upload_buffer, upload_buffer_memory,
            &upload_buffer_memory_type)) {
      REXUI_ERROR(
          "VulkanImmediateDrawer: Failed to create an upload buffer for a "
          "{}x{} image",
          width, height);
      FreeTextureDescriptor(descriptor_index);
      dfn.vkDestroyImageView(device, image_view, nullptr);
      dfn.vkDestroyImage(device, image, nullptr);
      dfn.vkFreeMemory(device, image_memory, nullptr);
      return false;
    }
    void* upload_buffer_mapping;
    if (dfn.vkMapMemory(device, upload_buffer_memory, 0, VK_WHOLE_SIZE, 0,
                        &upload_buffer_mapping) != VK_SUCCESS) {
      REXUI_ERROR(
          "VulkanImmediateDrawer: Failed to map upload buffer memory for a "
          "{}x{} image",
          width, height);
      dfn.vkDestroyBuffer(device, upload_buffer, nullptr);
      dfn.vkFreeMemory(device, upload_buffer_memory, nullptr);
      FreeTextureDescriptor(descriptor_index);
      dfn.vkDestroyImageView(device, image_view, nullptr);
      dfn.vkDestroyImage(device, image, nullptr);
      dfn.vkFreeMemory(device, image_memory, nullptr);
      return false;
    }
    std::memcpy(upload_buffer_mapping, data, data_size);
    objects::FlushMappedMemoryRange(vulkan_device_, upload_buffer_memory,
                                    upload_buffer_memory_type);
    dfn.vkUnmapMemory(device, upload_buffer_memory);
  }

  resource_out.image = image;
  resource_out.memory = image_memory;
  resource_out.image_view = image_view;
  resource_out.descriptor_index = descriptor_index;

  pending_upload_index_out = texture_uploads_pending_.size();
  PendingTextureUpload& pending_upload = texture_uploads_pending_.emplace_back();
  pending_upload.texture = nullptr;
  pending_upload.buffer = upload_buffer;
  pending_upload.buffer_memory = upload_buffer_memory;
  pending_upload.image = image;
  pending_upload.width = width;
  pending_upload.height = height;

  return true;
}

void VulkanImmediateDrawer::DestroyTextureResource(VulkanOverlayTexture::Resource& resource) {
  FreeTextureDescriptor(resource.descriptor_index);
  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();
  dfn.vkDestroyImageView(device, resource.image_view, nullptr);
  dfn.vkDestroyImage(device, resource.image, nullptr);
  dfn.vkFreeMemory(device, resource.memory, nullptr);
}

void VulkanImmediateDrawer::OnImmediateTextureDestroyed(VulkanOverlayTexture& texture) {
  size_t pending_upload_index = texture.pending_upload_index;
  if (pending_upload_index != SIZE_MAX) {
    if (pending_upload_index + 1 < texture_uploads_pending_.size()) {
      PendingTextureUpload& pending_upload = texture_uploads_pending_[pending_upload_index];
      pending_upload = texture_uploads_pending_.back();
      if (pending_upload.texture) {
        pending_upload.texture->pending_upload_index = pending_upload_index;
      }
    }
    texture_uploads_pending_.pop_back();
  }

  VulkanOverlayTexture*& texture_at_index = textures_[texture.immediate_drawer_index];
  texture_at_index = textures_.back();
  texture_at_index->immediate_drawer_index = texture.immediate_drawer_index;
  textures_.pop_back();

  if (texture.last_usage_submission > last_completed_submission_index_) {
    textures_deleted_.emplace_back(std::make_pair(texture.resource, texture.last_usage_submission));
  } else {
    DestroyTextureResource(texture.resource);
  }
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
