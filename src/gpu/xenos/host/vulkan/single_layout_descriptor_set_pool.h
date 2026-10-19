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

#include <cstddef>
#include <cstdint>
#include <vector>

#include <rex/assert.h>
#include "gpu/xenos/host/vulkan/device.h"

namespace rex {
namespace gpu {
namespace vulkan {

class SingleLayoutDescriptorSetPool {
 public:
  /**
   * Creates the pool.
   *
   * The set layout must outlive this object.
   *
   * @param vulkan_device The device to allocate on.
   * @param pool_set_count The number of sets in each pool.
   * @param set_layout_descriptor_counts_count The number of entries in the counts array.
   * @param set_layout_descriptor_counts The descriptors of each type in a single set.
   * @param set_layout The layout of every set allocated from the pool.
   */
  SingleLayoutDescriptorSetPool(const VulkanDevice* vulkan_device, u32 pool_set_count,
                                u32 set_layout_descriptor_counts_count,
                                const VkDescriptorPoolSize* set_layout_descriptor_counts,
                                VkDescriptorSetLayout set_layout);
  ~SingleLayoutDescriptorSetPool();

  /**
   * Allocates a descriptor set.
   *
   * @return The set index, or SIZE_MAX on failure.
   */
  size_t Allocate();
  void Free(size_t index) noexcept {
    REX_ASSERT(index < descriptor_sets_.size());
    next_free_[index] = free_head_;
    free_head_ = index;
  }
  VkDescriptorSet Get(size_t index) const { return descriptor_sets_[index]; }

 private:
  const VulkanDevice* vulkan_device_;
  u32 pool_set_count_;
  std::vector<VkDescriptorPoolSize> pool_descriptor_counts_;
  VkDescriptorSetLayout set_layout_;

  std::vector<VkDescriptorPool> full_pools_;
  VkDescriptorPool current_pool_ = VK_NULL_HANDLE;
  u32 current_pool_sets_remaining_ = 0;

  std::vector<VkDescriptorSet> descriptor_sets_;
  std::vector<size_t> next_free_;
  size_t free_head_ = SIZE_MAX;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
