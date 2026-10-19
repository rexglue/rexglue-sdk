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

#include "gpu/xenos/host/graphics_upload_buffer_pool.h"
#include "gpu/xenos/host/vulkan/device.h"

namespace rex {
namespace gpu {
namespace vulkan {

class VulkanUploadBufferPool : public GraphicsUploadBufferPool {
 public:
  /**
   * Creates the pool.
   *
   * The page size is rounded up to nonCoherentAtomSize, the memory mapping granularity, so the
   * padding of host visible allocations is not wasted.
   * Map and flush ranges must be clamped to the allocation size.
   *
   * @param vulkan_device The device to allocate on.
   * @param usage The buffer usage flags.
   * @param page_size The page size in bytes before rounding.
   */
  VulkanUploadBufferPool(const VulkanDevice* vulkan_device, VkBufferUsageFlags usage,
                         size_t page_size = kDefaultPageSize);

  u8* Request(u64 submission_index, size_t size, size_t alignment, VkBuffer& buffer_out,
              VkDeviceSize& offset_out);
  u8* RequestPartial(u64 submission_index, size_t size, size_t alignment, VkBuffer& buffer_out,
                     VkDeviceSize& offset_out, VkDeviceSize& size_out);

 protected:
  std::unique_ptr<Page> CreatePageImplementation() override;

  void FlushPageWrites(Page* page, size_t offset, size_t size) override;

 private:
  struct VulkanPage : public Page {
    /**
     * Creates the page and takes ownership of the buffer, its memory and its mapping.
     *
     * @param vulkan_device The owning device.
     * @param buffer The page buffer.
     * @param memory The buffer memory.
     * @param mapping The mapped pointer of the memory.
     */
    VulkanPage(const VulkanDevice* vulkan_device, VkBuffer buffer, VkDeviceMemory memory,
               void* mapping)
        : vulkan_device(vulkan_device), buffer(buffer), memory(memory), mapping(mapping) {}
    ~VulkanPage() override;
    const VulkanDevice* vulkan_device;
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* mapping;
  };

  const VulkanDevice* vulkan_device_;

  VkDeviceSize allocation_size_;
  static constexpr u32 kMemoryTypeUnknown = UINT32_MAX;
  static constexpr u32 kMemoryTypeUnavailable = kMemoryTypeUnknown - 1;
  u32 memory_type_ = UINT32_MAX;

  VkBufferUsageFlags usage_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
