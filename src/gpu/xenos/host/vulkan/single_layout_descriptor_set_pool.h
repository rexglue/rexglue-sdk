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
   * @param set_layout The set layout.
   */
  SingleLayoutDescriptorSetPool(const VulkanDevice* vulkan_device, uint32_t pool_set_count,
                                uint32_t set_layout_descriptor_counts_count,
                                const VkDescriptorPoolSize* set_layout_descriptor_counts,
                                VkDescriptorSetLayout set_layout);
  ~SingleLayoutDescriptorSetPool();

  /**
   * Allocates a descriptor set.
   *
   * @return The set index, or SIZE_MAX on failure.
   */
  size_t Allocate();
  void Free(size_t index) {
    REX_ASSERT(index < descriptor_sets_.size());
    descriptor_sets_free_.push_back(index);
  }
  VkDescriptorSet Get(size_t index) const { return descriptor_sets_[index]; }

 private:
  const VulkanDevice* vulkan_device_;
  uint32_t pool_set_count_;
  std::vector<VkDescriptorPoolSize> pool_descriptor_counts_;
  VkDescriptorSetLayout set_layout_;

  std::vector<VkDescriptorPool> full_pools_;
  VkDescriptorPool current_pool_ = VK_NULL_HANDLE;
  uint32_t current_pool_sets_remaining_ = 0;

  std::vector<VkDescriptorSet> descriptor_sets_;
  std::vector<size_t> descriptor_sets_free_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
