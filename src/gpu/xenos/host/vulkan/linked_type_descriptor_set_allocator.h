#pragma once
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

// Allocates multiple descriptors of in descriptor set layouts consisting of
// descriptors of types specified during initialization.
//
// "LinkedType" means that the allocator is designed for allocating descriptor
// sets containing descriptors of multiple types together - for instance, it
// will mark the entire page as full even if no space is left in it for only one
// of the descriptor types (not all at once).
//
// The primary usage scenario for this allocator is allocating image
// and sampler descriptors in a single descriptor set if they both are
// used in one. It is expected that the ratio of the numbers of descriptors per
// type specified during the initialization will roughly correspond to the ratio
// of the numbers of descriptors that will be allocated. For instance,
// if there are approximately 2 images for each 1 sampler, it's recommended to
// make the image count per page twice the sampler count per page.
//
// If some allocations use only one type, and some use only another, completely
// independently, it's preferable to use separate allocators rather than a
// single one.
//
// This allocator is also suitable for allocating variable-length descriptor
// sets containing descriptors of only a single type.
//
// There's no way to free these descriptors within the allocator object itself,
// per-layout free lists should be used externally.
class LinkedTypeDescriptorSetAllocator : public rex::NonCopyable {
 public:
  // Multiple descriptor sizes for the same descriptor type, and zero sizes, are
  // not allowed.
  explicit LinkedTypeDescriptorSetAllocator(const VulkanDevice* const vulkan_device,
                                            const VkDescriptorPoolSize* const descriptor_sizes,
                                            const uint32_t descriptor_size_count,
                                            const uint32_t descriptor_sets_per_page)
      : vulkan_device_(vulkan_device),
        descriptor_pool_sizes_(
            std::make_unique_for_overwrite<VkDescriptorPoolSize[]>(descriptor_size_count)),
        descriptor_pool_size_count_(descriptor_size_count),
        descriptor_sets_per_page_(descriptor_sets_per_page) {
    REX_ASSERT(vulkan_device != nullptr);
    REX_ASSERT(descriptor_size_count != 0);
    REX_ASSERT(descriptor_sets_per_page_ != 0);
#ifndef NDEBUG
    for (uint32_t i = 0; i < descriptor_size_count; ++i) {
      const VkDescriptorPoolSize& descriptor_size = descriptor_sizes[i];
      REX_ASSERT(descriptor_size.descriptorCount != 0);
      for (uint32_t j = 0; j < i; ++j) {
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
                           uint32_t descriptor_type_count);

 private:
  struct Page {
    VkDescriptorPool pool;
    std::unique_ptr<VkDescriptorPoolSize[]> descriptors_remaining;
    uint32_t descriptor_sets_remaining;
  };

  const VulkanDevice* vulkan_device_;

  std::unique_ptr<VkDescriptorPoolSize[]> descriptor_pool_sizes_;
  uint32_t descriptor_pool_size_count_;
  uint32_t descriptor_sets_per_page_;

  std::vector<VkDescriptorPool> pages_full_;
  // Because allocations must be contiguous, overflow may happen even if a page
  // still has free descriptors, so multiple pages may have free space.
  // To avoid removing and re-adding the page to the map that keeps them sorted
  // (the key is the maximum number of free descriptors remaining across all
  // types - and lookups need to be made with the maximum of the requested
  // number of descriptors across all types since it's pointless to check the
  // pages that can't even potentially fit the largest amount of descriptors of
  // a requested type, and unlike using the minimum as the key, this doesn't
  // degenerate if, for example, 0 descriptors are requested for some type - and
  // it changes at every allocation from a page), instead of always looking for
  // a free space in the map, maintaining one page outside the map, and
  // allocation attempts will be made from that page first.
  std::multimap<uint32_t, Page> pages_usable_;
  // Doesn't exist if page_usable_latest_.pool == VK_NULL_HANDLE.
  Page page_usable_latest_ = {};
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
