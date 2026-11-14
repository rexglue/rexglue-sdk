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

#include <algorithm>
#include <cstdint>
#include <memory>

#include <rex/assert.h>
#include "gpu/xenos/vulkan/command_processor.h"
#include "gpu/xenos/vulkan/deferred_command_buffer.h"
#include "gpu/xenos/vulkan/primitive_processor.h"
#include <rex/logging/log_category.h>
#include "gpu/xenos/host/vulkan/vulkan_objects.h"

namespace rex::gpu::vulkan {

VulkanPrimitiveProcessor::~VulkanPrimitiveProcessor() {
  Shutdown(true);
}

bool VulkanPrimitiveProcessor::Initialize() {
  const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->GetProperties();
  constexpr bool kTriangleFansSupportedWithoutConversion = false;

  bool geometry_shader_primitive_emulation_allowed = device_properties.geometryShader;
  bool quad_lists_supported_without_conversion =
      geometry_shader_primitive_emulation_allowed &&
      !command_processor_.GetVulkanSettings().force_convert_quad_lists_to_triangle_lists.Get();
  bool point_sprites_supported_without_vs_expansion =
      geometry_shader_primitive_emulation_allowed &&
      !command_processor_.GetVulkanSettings().force_expand_point_sprites_in_vs.Get();
  bool rectangle_lists_supported_without_vs_expansion =
      geometry_shader_primitive_emulation_allowed &&
      !command_processor_.GetVulkanSettings().force_expand_rectangle_lists_in_vs.Get();

  if (!InitializeCommon(
          device_properties.fullDrawIndexUint32, kTriangleFansSupportedWithoutConversion, false,
          quad_lists_supported_without_conversion, point_sprites_supported_without_vs_expansion,
          rectangle_lists_supported_without_vs_expansion)) {
    Shutdown();
    return false;
  }
  frame_index_buffer_pool_ = std::make_unique<gpu::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      std::max(static_cast<size_t>(kMinRequiredConvertedIndexBufferSize),
               gpu::GraphicsUploadBufferPool::kDefaultPageSize));
  return true;
}

void VulkanPrimitiveProcessor::Shutdown(bool from_destructor) {
  const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();

  frame_index_buffers_.clear();
  frame_index_buffer_pool_.reset();
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                             builtin_index_buffer_upload_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                             builtin_index_buffer_upload_memory_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, builtin_index_buffer_);
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                             builtin_index_buffer_memory_);

  if (!from_destructor) {
    ShutdownCommon();
  }
}

void VulkanPrimitiveProcessor::CompletedSubmissionUpdated() {
  if (builtin_index_buffer_upload_ != VK_NULL_HANDLE &&
      command_processor_.GetCompletedSubmission() >= builtin_index_buffer_upload_submission_) {
    const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
    const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
    const VkDevice device = vulkan_device->GetDevice();
    gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                               builtin_index_buffer_upload_);
    gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                               builtin_index_buffer_upload_memory_);
  }
}

void VulkanPrimitiveProcessor::BeginSubmission() {
  if (builtin_index_buffer_upload_ != VK_NULL_HANDLE &&
      builtin_index_buffer_upload_submission_ == UINT64_MAX) {
    // No deferred barriers needed, the index buffer is unused and the upload is written first.

    command_processor_.EndRenderPass();

    DeferredCommandBuffer& command_buffer = command_processor_.GetDeferredCommandBuffer();

    VkBufferCopy* copy_region = command_buffer.CmdVkCopyBufferEmplace(builtin_index_buffer_upload_,
                                                                      builtin_index_buffer_, 1);
    copy_region->srcOffset = 0;
    copy_region->dstOffset = 0;
    copy_region->size = builtin_index_buffer_size_;

    command_processor_.PushBufferMemoryBarrier(
        builtin_index_buffer_, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_INDEX_READ_BIT);

    builtin_index_buffer_upload_submission_ = command_processor_.GetCurrentSubmission();
  }
}

void VulkanPrimitiveProcessor::BeginFrame() {
  frame_index_buffer_pool_->Reclaim(command_processor_.GetCompletedFrame());
}

void VulkanPrimitiveProcessor::EndSubmission() {
  frame_index_buffer_pool_->FlushWrites();
}

void VulkanPrimitiveProcessor::EndFrame() {
  ClearPerFrameCache();
  frame_index_buffers_.clear();
}

bool VulkanPrimitiveProcessor::InitializeBuiltinIndexBuffer(
    size_t size_bytes, std::function<void(void*)> fill_callback) {
  REX_ASSERT(size_bytes != 0);
  REX_ASSERT(builtin_index_buffer_ == VK_NULL_HANDLE);
  REX_ASSERT(builtin_index_buffer_memory_ == VK_NULL_HANDLE);
  REX_ASSERT(builtin_index_buffer_upload_ == VK_NULL_HANDLE);
  REX_ASSERT(builtin_index_buffer_upload_memory_ == VK_NULL_HANDLE);

  const gpu::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const gpu::vulkan::VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();

  builtin_index_buffer_size_ = static_cast<VkDeviceSize>(size_bytes);
  if (!gpu::vulkan::objects::CreateDedicatedAllocationBuffer(
          vulkan_device, builtin_index_buffer_size_,
          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
          gpu::vulkan::objects::MemoryPurpose::kDeviceLocal, builtin_index_buffer_,
          builtin_index_buffer_memory_)) {
    REXGPU_FAIL(false,
                "Vulkan primitive processor: Failed to create the built-in index "
                "buffer GPU resource with {} bytes",
                size_bytes);
  }
  u32 upload_memory_type;
  if (!gpu::vulkan::objects::CreateDedicatedAllocationBuffer(
          vulkan_device, builtin_index_buffer_size_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          gpu::vulkan::objects::MemoryPurpose::kUpload, builtin_index_buffer_upload_,
          builtin_index_buffer_upload_memory_, &upload_memory_type)) {
    REXGPU_ERROR(
        "Vulkan primitive processor: Failed to create the built-in index "
        "buffer upload resource with {} bytes",
        size_bytes);
    gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, builtin_index_buffer_);
    gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                               builtin_index_buffer_memory_);
    return false;
  }

  void* mapping;
  if (dfn.vkMapMemory(device, builtin_index_buffer_upload_memory_, 0, VK_WHOLE_SIZE, 0, &mapping) !=
      VK_SUCCESS) {
    REXGPU_ERROR(
        "Vulkan primitive processor: Failed to map the built-in index buffer "
        "upload resource with {} bytes",
        size_bytes);
    gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                               builtin_index_buffer_upload_);
    gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                               builtin_index_buffer_upload_memory_);
    gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, builtin_index_buffer_);
    gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                               builtin_index_buffer_memory_);
    return false;
  }
  fill_callback(mapping);
  gpu::vulkan::objects::FlushMappedMemoryRange(vulkan_device, builtin_index_buffer_memory_,
                                               upload_memory_type);
  dfn.vkUnmapMemory(device, builtin_index_buffer_upload_memory_);

  builtin_index_buffer_upload_submission_ = UINT64_MAX;
  return true;
}

void* VulkanPrimitiveProcessor::RequestHostConvertedIndexBufferForCurrentFrame(
    xenos::IndexFormat format, u32 index_count, bool coalign_for_simd,
    u32 coalignment_original_address, size_t& driver_handle_out) {
  size_t index_size = format == xenos::IndexFormat::kInt16 ? sizeof(u16) : sizeof(u32);
  VkBuffer buffer;
  VkDeviceSize offset;
  u8* mapping = frame_index_buffer_pool_->Request(
      command_processor_.GetCurrentFrame(),
      index_size * index_count + (coalign_for_simd ? REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE : 0),
      index_size, buffer, offset);
  if (!mapping) {
    return nullptr;
  }
  if (coalign_for_simd) {
    ptrdiff_t coalignment_offset = GetSIMDCoalignmentOffset(mapping, coalignment_original_address);
    mapping += coalignment_offset;
    offset = static_cast<VkDeviceSize>(offset + coalignment_offset);
  }
  driver_handle_out = frame_index_buffers_.size();
  frame_index_buffers_.emplace_back(buffer, offset);
  return mapping;
}

}  // namespace rex::gpu::vulkan
