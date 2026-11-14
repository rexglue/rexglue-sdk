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
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include <rex/assert.h>
#include <rex/types.h>
#include <rex/utility.h>
#include "gpu/xenos/host/vulkan/vulkan_device.h"

namespace rex {
namespace gpu {
namespace vulkan {

/**
 * Allocates descriptor sets spanning the types given at construction.
 *
 * A page is full as soon as any one type runs out, so per page counts should match the allocated
 * ratio, for example two images per sampler. Independent types need separate allocators.
 * Descriptors cannot be freed, so per layout free lists live outside the allocator.
 */
class LinkedTypeDescriptorSetAllocator : public rex::NonCopyable {
 public:
  /**
   * Creates the allocator. Each type needs exactly one nonzero size.
   *
   * @param vulkan_device The device to allocate on.
   * @param descriptor_sizes The descriptors per page for each type.
   * @param descriptor_size_count The number of entries in descriptor_sizes.
   * @param descriptor_sets_per_page The maximum number of sets in a page.
   */
  explicit LinkedTypeDescriptorSetAllocator(const VulkanDevice* const vulkan_device,
                                            const VkDescriptorPoolSize* const descriptor_sizes,
                                            const u32 descriptor_size_count,
                                            const u32 descriptor_sets_per_page)
      : vulkan_device_(vulkan_device),
        descriptor_pool_sizes_(
            std::make_unique_for_overwrite<VkDescriptorPoolSize[]>(descriptor_size_count)),
        descriptor_pool_size_count_(descriptor_size_count),
        descriptor_sets_per_page_(descriptor_sets_per_page) {
    REX_ASSERT(vulkan_device != nullptr);
    REX_ASSERT(descriptor_size_count != 0);
    REX_ASSERT(descriptor_sets_per_page_ != 0);
#ifndef NDEBUG
    for (u32 i = 0; i < descriptor_size_count; ++i) {
      const VkDescriptorPoolSize& descriptor_size = descriptor_sizes[i];
      REX_ASSERT(descriptor_size.descriptorCount != 0);
      for (u32 j = 0; j < i; ++j) {
        REX_ASSERT(descriptor_sizes[j].type != descriptor_size.type);
      }
    }
#endif
    std::memcpy(descriptor_pool_sizes_.get(), descriptor_sizes,
                sizeof(VkDescriptorPoolSize) * descriptor_size_count);
  }
  ~LinkedTypeDescriptorSetAllocator() { Reset(); }

  void Reset();

  VkDescriptorSet Allocate(VkDescriptorSetLayout descriptor_set_layout,
                           const VkDescriptorPoolSize* descriptor_counts,
                           u32 descriptor_type_count);

 private:
  struct Page {
    VkDescriptorPool pool;
    std::unique_ptr<VkDescriptorPoolSize[]> descriptors_remaining;
    u32 descriptor_sets_remaining;
  };

  const VulkanDevice* vulkan_device_;

  std::unique_ptr<VkDescriptorPoolSize[]> descriptor_pool_sizes_;
  u32 descriptor_pool_size_count_;
  u32 descriptor_sets_per_page_;

  std::vector<VkDescriptorPool> pages_full_;
  /** Pages with free descriptors, keyed by the maximum free count across all types. */
  std::multimap<u32, Page> pages_usable_;
  /** Tried first and kept outside the map. Absent when its pool is VK_NULL_HANDLE. */
  Page page_usable_latest_ = {};
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
