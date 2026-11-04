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

#include <rex/logging/log.h>
#include <rex/math.h>
#include "gpu/xenos/host/vulkan/upload_buffer_pool.h"
#include "gpu/xenos/host/vulkan/objects.h"

namespace rex {
namespace gpu {
namespace vulkan {

VulkanUploadBufferPool::VulkanUploadBufferPool(const VulkanDevice* const vulkan_device,
                                               const VkBufferUsageFlags usage,
                                               const size_t page_size)
    : GraphicsUploadBufferPool(static_cast<size_t>(
          rex::RoundUpNonZero(static_cast<VkDeviceSize>(page_size),
                              vulkan_device->GetProperties().nonCoherentAtomSize))),
      vulkan_device_(vulkan_device),
      usage_(usage) {}

u8* VulkanUploadBufferPool::Request(u64 submission_index, size_t size, size_t alignment,
                                    VkBuffer& buffer_out, VkDeviceSize& offset_out) {
  size_t offset;
  const VulkanPage* page = static_cast<const VulkanPage*>(
      GraphicsUploadBufferPool::Request(submission_index, size, alignment, offset));
  if (!page) {
    return nullptr;
  }
  buffer_out = page->buffer;
  offset_out = static_cast<VkDeviceSize>(offset);
  return reinterpret_cast<u8*>(page->mapping) + offset;
}

u8* VulkanUploadBufferPool::RequestPartial(u64 submission_index, size_t size, size_t alignment,
                                           VkBuffer& buffer_out, VkDeviceSize& offset_out,
                                           VkDeviceSize& size_out) {
  size_t offset, size_obtained;
  const VulkanPage* page = static_cast<const VulkanPage*>(GraphicsUploadBufferPool::RequestPartial(
      submission_index, size, alignment, offset, size_obtained));
  if (!page) {
    return nullptr;
  }
  buffer_out = page->buffer;
  offset_out = static_cast<VkDeviceSize>(offset);
  size_out = static_cast<VkDeviceSize>(size_obtained);
  return reinterpret_cast<u8*>(page->mapping) + offset;
}

std::unique_ptr<GraphicsUploadBufferPool::Page> VulkanUploadBufferPool::CreatePageImplementation() {
  if (memory_type_ == kMemoryTypeUnavailable) {
    // Never retry creation once the memory type is unavailable.
    return nullptr;
  }

  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();

  VkBufferCreateInfo buffer_create_info;
  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.pNext = nullptr;
  buffer_create_info.flags = 0;
  buffer_create_info.size = static_cast<VkDeviceSize>(page_size_);
  buffer_create_info.usage = usage_;
  buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  buffer_create_info.queueFamilyIndexCount = 0;
  buffer_create_info.pQueueFamilyIndices = nullptr;
  VkBuffer buffer;
  if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer) != VK_SUCCESS) {
    REXUI_FAIL(nullptr, "Failed to create a Vulkan upload buffer with {} bytes", page_size_);
  }

  if (memory_type_ == kMemoryTypeUnknown) {
    VkMemoryRequirements memory_requirements;
    dfn.vkGetBufferMemoryRequirements(device, buffer, &memory_requirements);
    memory_type_ = objects::ChooseHostMemoryType(vulkan_device_->GetMemoryTypes(),
                                                 memory_requirements.memoryTypeBits, false);
    if (memory_type_ == UINT32_MAX) {
      REXUI_ERROR(
          "No host-visible memory types can store a Vulkan upload buffer with "
          "{} bytes",
          page_size_);
      memory_type_ = kMemoryTypeUnavailable;
      dfn.vkDestroyBuffer(device, buffer, nullptr);
      return nullptr;
    }
    allocation_size_ = memory_requirements.size;
    if (allocation_size_ > page_size_) {
      // Occupy the allocation padding unless that needs more memory.
      buffer_create_info.size = allocation_size_;
      VkBuffer buffer_expanded;
      if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_expanded) ==
          VK_SUCCESS) {
        VkMemoryRequirements memory_requirements_expanded;
        dfn.vkGetBufferMemoryRequirements(device, buffer_expanded, &memory_requirements_expanded);
        u32 memory_type_expanded = objects::ChooseHostMemoryType(
            vulkan_device_->GetMemoryTypes(), memory_requirements.memoryTypeBits, false);
        if (memory_requirements_expanded.size <= allocation_size_ &&
            memory_type_expanded != UINT32_MAX) {
          page_size_ = static_cast<size_t>(allocation_size_);
          allocation_size_ = memory_requirements_expanded.size;
          memory_type_ = memory_type_expanded;
          dfn.vkDestroyBuffer(device, buffer, nullptr);
          buffer = buffer_expanded;
        } else {
          dfn.vkDestroyBuffer(device, buffer_expanded, nullptr);
        }
      }
    }
  }

  VkMemoryAllocateInfo memory_allocate_info;
  VkMemoryAllocateInfo* memory_allocate_info_last = &memory_allocate_info;
  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.pNext = nullptr;
  memory_allocate_info.allocationSize = allocation_size_;
  memory_allocate_info.memoryTypeIndex = memory_type_;
  VkMemoryDedicatedAllocateInfo memory_dedicated_allocate_info;
  if (vulkan_device_->GetExtensions().ext_1_1_KHR_dedicated_allocation) {
    memory_allocate_info_last->pNext = &memory_dedicated_allocate_info;
    memory_allocate_info_last =
        reinterpret_cast<VkMemoryAllocateInfo*>(&memory_dedicated_allocate_info);
    memory_dedicated_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    memory_dedicated_allocate_info.pNext = nullptr;
    memory_dedicated_allocate_info.image = VK_NULL_HANDLE;
    memory_dedicated_allocate_info.buffer = buffer;
  }
  VkDeviceMemory memory;
  if (dfn.vkAllocateMemory(device, &memory_allocate_info, nullptr, &memory) != VK_SUCCESS) {
    REXUI_ERROR("Failed to allocate {} bytes of Vulkan upload buffer memory", allocation_size_);
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    return nullptr;
  }

  if (dfn.vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
    REXUI_ERROR("Failed to bind memory to a Vulkan upload buffer with {} bytes", page_size_);
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
    return nullptr;
  }

  void* mapping;
  if (dfn.vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS) {
    REXUI_ERROR("Failed to map {} bytes of Vulkan upload buffer memory", allocation_size_);
    dfn.vkDestroyBuffer(device, buffer, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
    return nullptr;
  }

  return std::make_unique<VulkanPage>(vulkan_device_, buffer, memory, mapping);
}

void VulkanUploadBufferPool::FlushPageWrites(Page* page, size_t offset, size_t size) {
  objects::FlushMappedMemoryRange(vulkan_device_, static_cast<const VulkanPage*>(page)->memory,
                                  memory_type_, static_cast<VkDeviceSize>(offset), allocation_size_,
                                  static_cast<VkDeviceSize>(size));
}

VulkanUploadBufferPool::VulkanPage::~VulkanPage() {
  const VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();
  dfn.vkDestroyBuffer(device, buffer, nullptr);
  // Freeing the memory unmaps it implicitly.
  dfn.vkFreeMemory(device, memory, nullptr);
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
