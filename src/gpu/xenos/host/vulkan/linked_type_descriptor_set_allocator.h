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
#include "gpu/xenos/host/vulkan/device.h"

namespace rex {
namespace gpu {
namespace vulkan {

/**
 * Allocates descriptor sets whose descriptors span the types given at construction.
 *
 * Linked type means a page is marked full as soon as any one descriptor type runs out. The
 * per page descriptor counts per type should roughly match the ratio allocated, for example
 * twice as many images as samplers when sets use 2 images per sampler. Allocations that use
 * independent types belong in separate allocators. Also suits variable length sets of a single
 * type. Descriptors cannot be freed here, so per layout free lists live outside the allocator.
 */
class LinkedTypeDescriptorSetAllocator : public rex::NonCopyable {
 public:
  /**
   * Creates the allocator.
   *
   * Multiple sizes for the same descriptor type, and zero sizes, are not allowed.
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
  /**
   * Pages with free descriptors, keyed by the maximum free descriptor count across all types.
   *
   * Allocations must be contiguous, so a page can overflow while descriptors remain and several
   * pages may have free space. The maximum rather than the minimum is the key so lookups skip
   * pages that cannot fit the largest requested count and do not degenerate when a type requests
   * 0. One page, page_usable_latest_, is kept outside the map and tried first, since the key
   * changes on every allocation.
   */
  std::multimap<u32, Page> pages_usable_;
  /** Absent when its pool is VK_NULL_HANDLE. */
  Page page_usable_latest_ = {};
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
