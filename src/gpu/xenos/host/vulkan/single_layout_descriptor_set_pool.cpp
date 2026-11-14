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

#include <rex/assert.h>
#include <rex/logging/log_category.h>
#include "gpu/xenos/host/vulkan/single_layout_descriptor_set_pool.h"

namespace rex {
namespace gpu {
namespace vulkan {

SingleLayoutDescriptorSetPool::SingleLayoutDescriptorSetPool(
    const VulkanDevice* const vulkan_device, const u32 pool_set_count,
    const u32 set_layout_descriptor_counts_count,
    const VkDescriptorPoolSize* const set_layout_descriptor_counts,
    const VkDescriptorSetLayout set_layout)
    : vulkan_device_(vulkan_device), pool_set_count_(pool_set_count), set_layout_(set_layout) {
  REX_ASSERT(vulkan_device != nullptr);
  REX_ASSERT(pool_set_count != 0);
  pool_descriptor_counts_.resize(set_layout_descriptor_counts_count);
  for (u32 i = 0; i < set_layout_descriptor_counts_count; ++i) {
    VkDescriptorPoolSize& pool_descriptor_type_count = pool_descriptor_counts_[i];
    const VkDescriptorPoolSize& set_layout_descriptor_type_count = set_layout_descriptor_counts[i];
    pool_descriptor_type_count.type = set_layout_descriptor_type_count.type;
    pool_descriptor_type_count.descriptorCount =
        set_layout_descriptor_type_count.descriptorCount * pool_set_count;
  }
}

SingleLayoutDescriptorSetPool::~SingleLayoutDescriptorSetPool() {
  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();
  if (current_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyDescriptorPool(device, current_pool_, nullptr);
  }
  for (VkDescriptorPool pool : full_pools_) {
    dfn.vkDestroyDescriptorPool(device, pool, nullptr);
  }
}

size_t SingleLayoutDescriptorSetPool::Allocate() {
  if (free_head_ != SIZE_MAX) {
    const size_t free_index = free_head_;
    free_head_ = next_free_[free_index];
    return free_index;
  }

  descriptor_sets_.reserve(descriptor_sets_.size() + 1);
  next_free_.reserve(descriptor_sets_.size() + 1);
  full_pools_.reserve(full_pools_.size() + 1);

  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();

  // 2 attempts so a failure with sets remaining retries in a new pool.
  for (u32 i = 0; i < 2; ++i) {
    if (current_pool_ != VK_NULL_HANDLE && !current_pool_sets_remaining_) {
      full_pools_.push_back(current_pool_);
      current_pool_ = VK_NULL_HANDLE;
    }
    if (current_pool_ == VK_NULL_HANDLE) {
      VkDescriptorPoolCreateInfo pool_create_info;
      pool_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
      pool_create_info.pNext = nullptr;
      pool_create_info.flags = 0;
      pool_create_info.maxSets = pool_set_count_;
      pool_create_info.poolSizeCount = static_cast<u32>(pool_descriptor_counts_.size());
      pool_create_info.pPoolSizes = pool_descriptor_counts_.data();
      if (dfn.vkCreateDescriptorPool(device, &pool_create_info, nullptr, &current_pool_) !=
          VK_SUCCESS) {
        REXUI_FAIL(SIZE_MAX,
                   "SingleLayoutDescriptorSetPool: Failed to create a descriptor "
                   "pool");
      }
      current_pool_sets_remaining_ = pool_set_count_;
    }

    VkDescriptorSetAllocateInfo descriptor_set_allocate_info;
    descriptor_set_allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptor_set_allocate_info.pNext = nullptr;
    descriptor_set_allocate_info.descriptorPool = current_pool_;
    descriptor_set_allocate_info.descriptorSetCount = 1;
    descriptor_set_allocate_info.pSetLayouts = &set_layout_;
    VkDescriptorSet descriptor_set;
    if (dfn.vkAllocateDescriptorSets(device, &descriptor_set_allocate_info, &descriptor_set) !=
        VK_SUCCESS) {
      REXUI_ERROR("SingleLayoutDescriptorSetPool: Failed to allocate a descriptor set");
      if (current_pool_sets_remaining_ >= pool_set_count_) {
        // Failed in a new pool, so the empty pool is destroyed rather than stored as full.
        dfn.vkDestroyDescriptorPool(device, current_pool_, nullptr);
        current_pool_ = VK_NULL_HANDLE;
        return SIZE_MAX;
      }
      full_pools_.push_back(current_pool_);
      current_pool_ = VK_NULL_HANDLE;
      continue;
    }
    --current_pool_sets_remaining_;
    descriptor_sets_.push_back(descriptor_set);
    next_free_.push_back(SIZE_MAX);
    return descriptor_sets_.size() - 1;
  }

  return SIZE_MAX;
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
