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
#include <iterator>
#include <utility>

#include <rex/assert.h>
#include <rex/logging/log_category.h>
#include "gpu/xenos/host/vulkan/linked_type_descriptor_set_allocator.h"
#include "gpu/xenos/host/vulkan/vulkan_objects.h"

namespace rex {
namespace gpu {
namespace vulkan {

void LinkedTypeDescriptorSetAllocator::Reset() {
  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();
  gpu::vulkan::objects::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                             page_usable_latest_.pool);
  page_usable_latest_.descriptors_remaining.reset();
  for (const std::pair<const u32, Page>& page_pair : pages_usable_) {
    dfn.vkDestroyDescriptorPool(device, page_pair.second.pool, nullptr);
  }
  pages_usable_.clear();
  for (VkDescriptorPool pool : pages_full_) {
    dfn.vkDestroyDescriptorPool(device, pool, nullptr);
  }
  pages_full_.clear();
}

VkDescriptorSet LinkedTypeDescriptorSetAllocator::Allocate(
    VkDescriptorSetLayout descriptor_set_layout, const VkDescriptorPoolSize* descriptor_counts,
    u32 descriptor_type_count) {
  REX_ASSERT(descriptor_type_count != 0);
#ifndef NDEBUG
  for (u32 i = 0; i < descriptor_type_count; ++i) {
    const VkDescriptorPoolSize& descriptor_count_for_type = descriptor_counts[i];
    REX_ASSERT(descriptor_count_for_type.descriptorCount != 0);
    for (u32 j = 0; j < i; ++j) {
      REX_ASSERT(descriptor_counts[j].type != descriptor_count_for_type.type);
    }
  }
#endif

  const VulkanDevice::Functions& dfn = vulkan_device_->GetFunctions();
  const VkDevice device = vulkan_device_->GetDevice();

  VkDescriptorSetAllocateInfo descriptor_set_allocate_info;
  descriptor_set_allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  descriptor_set_allocate_info.pNext = nullptr;
  descriptor_set_allocate_info.descriptorSetCount = 1;
  descriptor_set_allocate_info.pSetLayouts = &descriptor_set_layout;
  VkDescriptorSet descriptor_set;

  bool dedicated_descriptor_pool_needed = false;
  for (u32 i = 0; i < descriptor_type_count; ++i) {
    const VkDescriptorPoolSize& descriptor_count_for_type = descriptor_counts[i];
    dedicated_descriptor_pool_needed = true;
    for (u32 j = 0; j < descriptor_pool_size_count_; ++j) {
      const VkDescriptorPoolSize& descriptor_pool_size = descriptor_pool_sizes_[j];
      if (descriptor_count_for_type.type != descriptor_pool_size.type) {
        continue;
      }
      if (descriptor_count_for_type.descriptorCount <= descriptor_pool_size.descriptorCount) {
        dedicated_descriptor_pool_needed = false;
      }
      break;
    }
    if (dedicated_descriptor_pool_needed) {
      break;
    }
  }
  if (dedicated_descriptor_pool_needed) {
    VkDescriptorPoolCreateInfo dedicated_descriptor_pool_create_info;
    dedicated_descriptor_pool_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dedicated_descriptor_pool_create_info.pNext = nullptr;
    dedicated_descriptor_pool_create_info.flags = 0;
    dedicated_descriptor_pool_create_info.maxSets = 1;
    dedicated_descriptor_pool_create_info.poolSizeCount = descriptor_type_count;
    dedicated_descriptor_pool_create_info.pPoolSizes = descriptor_counts;
    VkDescriptorPool dedicated_descriptor_pool;
    if (dfn.vkCreateDescriptorPool(device, &dedicated_descriptor_pool_create_info, nullptr,
                                   &dedicated_descriptor_pool) != VK_SUCCESS) {
      REXUI_FAIL(VK_NULL_HANDLE,
                 "LinkedTypeDescriptorSetAllocator: Failed to create a dedicated "
                 "descriptor pool for a descriptor set that is too large for a pool "
                 "page");
    }
    descriptor_set_allocate_info.descriptorPool = dedicated_descriptor_pool;
    if (dfn.vkAllocateDescriptorSets(device, &descriptor_set_allocate_info, &descriptor_set) !=
        VK_SUCCESS) {
      REXUI_ERROR(
          "LinkedTypeDescriptorSetAllocator: Failed to allocate descriptors in "
          "a dedicated pool");
      dfn.vkDestroyDescriptorPool(device, dedicated_descriptor_pool, nullptr);
      return VK_NULL_HANDLE;
    }
    pages_full_.push_back(dedicated_descriptor_pool);
    return descriptor_set;
  }

  if (page_usable_latest_.pool != VK_NULL_HANDLE) {
    REX_ASSERT(page_usable_latest_.descriptor_sets_remaining != 0);
    bool allocate_from_latest_page = true;
    bool latest_page_becomes_full = page_usable_latest_.descriptor_sets_remaining == 1;
    for (u32 i = 0; i < descriptor_type_count; ++i) {
      const VkDescriptorPoolSize& descriptor_count_for_type = descriptor_counts[i];
      for (u32 j = 0; j < descriptor_pool_size_count_; ++j) {
        const VkDescriptorPoolSize& descriptors_remaining_for_type =
            page_usable_latest_.descriptors_remaining[j];
        if (descriptor_count_for_type.type != descriptors_remaining_for_type.type) {
          continue;
        }
        if (descriptor_count_for_type.descriptorCount >=
            descriptors_remaining_for_type.descriptorCount) {
          if (descriptor_count_for_type.descriptorCount >
              descriptors_remaining_for_type.descriptorCount) {
            allocate_from_latest_page = false;
            break;
          }
          latest_page_becomes_full = true;
        }
      }
      if (!allocate_from_latest_page) {
        break;
      }
    }
    if (allocate_from_latest_page) {
      descriptor_set_allocate_info.descriptorPool = page_usable_latest_.pool;
      if (dfn.vkAllocateDescriptorSets(device, &descriptor_set_allocate_info, &descriptor_set) !=
          VK_SUCCESS) {
        descriptor_set = VK_NULL_HANDLE;
        // Never allocate again from a pool that failed internally despite enough space.
        latest_page_becomes_full = true;
      }
      if (latest_page_becomes_full) {
        pages_full_.push_back(page_usable_latest_.pool);
        page_usable_latest_.pool = VK_NULL_HANDLE;
        page_usable_latest_.descriptors_remaining.reset();
      } else {
        --page_usable_latest_.descriptor_sets_remaining;
        for (u32 i = 0; i < descriptor_type_count; ++i) {
          const VkDescriptorPoolSize& descriptor_count_for_type = descriptor_counts[i];
          for (u32 j = 0; j < descriptor_pool_size_count_; ++j) {
            VkDescriptorPoolSize& descriptors_remaining_for_type =
                page_usable_latest_.descriptors_remaining[j];
            if (descriptor_count_for_type.type != descriptors_remaining_for_type.type) {
              continue;
            }
            descriptors_remaining_for_type.descriptorCount -=
                descriptor_count_for_type.descriptorCount;
          }
        }
      }
      if (descriptor_set != VK_NULL_HANDLE) {
        return descriptor_set;
      }
    }
  }

  u32 max_descriptors_per_type = descriptor_counts[0].descriptorCount;
  for (u32 i = 1; i < descriptor_type_count; ++i) {
    max_descriptors_per_type =
        std::max(max_descriptors_per_type, descriptor_counts[i].descriptorCount);
  }

  auto page_usable_it_next = pages_usable_.rbegin();
  while (page_usable_it_next != pages_usable_.rend()) {
    auto page_usable_it = page_usable_it_next;
    ++page_usable_it_next;
    if (page_usable_it->first < max_descriptors_per_type) {
      // Remaining entries have smaller keys, so none can fit.
      break;
    }
    bool map_page_has_sufficient_space = true;
    bool map_page_becomes_full = page_usable_it->second.descriptor_sets_remaining == 1;
    for (u32 i = 0; i < descriptor_type_count; ++i) {
      const VkDescriptorPoolSize& descriptor_count_for_type = descriptor_counts[i];
      for (u32 j = 0; j < descriptor_pool_size_count_; ++j) {
        const VkDescriptorPoolSize& descriptors_remaining_for_type =
            page_usable_it->second.descriptors_remaining[j];
        if (descriptor_count_for_type.type != descriptors_remaining_for_type.type) {
          continue;
        }
        if (descriptor_count_for_type.descriptorCount >=
            descriptors_remaining_for_type.descriptorCount) {
          if (descriptor_count_for_type.descriptorCount >
              descriptors_remaining_for_type.descriptorCount) {
            map_page_has_sufficient_space = false;
            break;
          }
          map_page_becomes_full = true;
        }
      }
      if (!map_page_has_sufficient_space) {
        break;
      }
    }
    if (!map_page_has_sufficient_space) {
      continue;
    }
    // Erase unconditionally as success changes the free counts that make up the map key.
    Page map_page = std::move(page_usable_it->second);
    pages_usable_.erase(std::next(page_usable_it).base());
    descriptor_set_allocate_info.descriptorPool = map_page.pool;
    if (dfn.vkAllocateDescriptorSets(device, &descriptor_set_allocate_info, &descriptor_set) !=
        VK_SUCCESS) {
      descriptor_set = VK_NULL_HANDLE;
      // Never allocate again from a pool that failed internally despite enough space.
      map_page_becomes_full = true;
    }
    if (map_page_becomes_full) {
      map_page.descriptors_remaining.reset();
      pages_full_.push_back(map_page.pool);
    } else {
      --map_page.descriptor_sets_remaining;
      for (u32 i = 0; i < descriptor_type_count; ++i) {
        const VkDescriptorPoolSize& descriptor_count_for_type = descriptor_counts[i];
        for (u32 j = 0; j < descriptor_pool_size_count_; ++j) {
          VkDescriptorPoolSize& descriptors_remaining_for_type = map_page.descriptors_remaining[j];
          if (descriptor_count_for_type.type != descriptors_remaining_for_type.type) {
            continue;
          }
          descriptors_remaining_for_type.descriptorCount -=
              descriptor_count_for_type.descriptorCount;
        }
      }
      if (page_usable_latest_.pool != VK_NULL_HANDLE) {
        u32 latest_page_max_descriptors_remaining =
            page_usable_latest_.descriptors_remaining[0].descriptorCount;
        for (u32 i = 1; i < descriptor_pool_size_count_; ++i) {
          latest_page_max_descriptors_remaining =
              std::max(latest_page_max_descriptors_remaining,
                       page_usable_latest_.descriptors_remaining[i].descriptorCount);
        }
        REX_ASSERT(latest_page_max_descriptors_remaining != 0);
        pages_usable_.emplace(latest_page_max_descriptors_remaining,
                              std::move(page_usable_latest_));
      }
      page_usable_latest_ = std::move(map_page);
    }
    if (descriptor_set != VK_NULL_HANDLE) {
      return descriptor_set;
    }
  }

  bool new_page_becomes_full = descriptor_sets_per_page_ == 1;
  for (u32 i = 0; !new_page_becomes_full && i < descriptor_type_count; ++i) {
    const VkDescriptorPoolSize& descriptor_count_for_type = descriptor_counts[i];
    for (u32 j = 0; j < descriptor_pool_size_count_; ++j) {
      const VkDescriptorPoolSize& descriptors_remaining_for_type = descriptor_pool_sizes_[j];
      if (descriptor_count_for_type.type != descriptors_remaining_for_type.type) {
        continue;
      }
      REX_ASSERT(descriptor_count_for_type.descriptorCount <=
                 descriptors_remaining_for_type.descriptorCount);
      if (descriptor_count_for_type.descriptorCount >=
          descriptors_remaining_for_type.descriptorCount) {
        new_page_becomes_full = true;
        break;
      }
    }
  }
  VkDescriptorPoolCreateInfo new_descriptor_pool_create_info;
  new_descriptor_pool_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  new_descriptor_pool_create_info.pNext = nullptr;
  new_descriptor_pool_create_info.flags = 0;
  if (new_page_becomes_full) {
    new_descriptor_pool_create_info.maxSets = 1;
    new_descriptor_pool_create_info.poolSizeCount = descriptor_type_count;
    new_descriptor_pool_create_info.pPoolSizes = descriptor_counts;
  } else {
    new_descriptor_pool_create_info.maxSets = descriptor_sets_per_page_;
    new_descriptor_pool_create_info.poolSizeCount = descriptor_pool_size_count_;
    new_descriptor_pool_create_info.pPoolSizes = descriptor_pool_sizes_.get();
  }
  VkDescriptorPool new_descriptor_pool;
  if (dfn.vkCreateDescriptorPool(device, &new_descriptor_pool_create_info, nullptr,
                                 &new_descriptor_pool) != VK_SUCCESS) {
    REXUI_FAIL(VK_NULL_HANDLE,
               "LinkedTypeDescriptorSetAllocator: Failed to create a descriptor pool");
  }
  descriptor_set_allocate_info.descriptorPool = new_descriptor_pool;
  if (dfn.vkAllocateDescriptorSets(device, &descriptor_set_allocate_info, &descriptor_set) !=
      VK_SUCCESS) {
    REXUI_ERROR("LinkedTypeDescriptorSetAllocator: Failed to allocate descriptors");
    dfn.vkDestroyDescriptorPool(device, new_descriptor_pool, nullptr);
    return VK_NULL_HANDLE;
  }
  if (new_page_becomes_full) {
    pages_full_.push_back(new_descriptor_pool);
  } else {
    if (page_usable_latest_.pool != VK_NULL_HANDLE) {
      u32 latest_page_max_descriptors_remaining =
          page_usable_latest_.descriptors_remaining[0].descriptorCount;
      for (u32 i = 1; i < descriptor_pool_size_count_; ++i) {
        latest_page_max_descriptors_remaining =
            std::max(latest_page_max_descriptors_remaining,
                     page_usable_latest_.descriptors_remaining[i].descriptorCount);
      }
      REX_ASSERT(latest_page_max_descriptors_remaining != 0);
      pages_usable_.emplace(latest_page_max_descriptors_remaining, std::move(page_usable_latest_));
    }
    page_usable_latest_.pool = new_descriptor_pool;
    page_usable_latest_.descriptors_remaining =
        std::make_unique_for_overwrite<VkDescriptorPoolSize[]>(descriptor_pool_size_count_);
    for (u32 i = 0; i < descriptor_pool_size_count_; ++i) {
      const VkDescriptorPoolSize& descriptor_pool_size_for_type = descriptor_pool_sizes_[i];
      page_usable_latest_.descriptors_remaining[i] = descriptor_pool_size_for_type;
      for (u32 j = 0; j < descriptor_type_count; ++j) {
        const VkDescriptorPoolSize& descriptor_count_for_type = descriptor_counts[j];
        if (descriptor_count_for_type.type != descriptor_pool_size_for_type.type) {
          continue;
        }
        page_usable_latest_.descriptors_remaining[i].descriptorCount -=
            descriptor_count_for_type.descriptorCount;
        break;
      }
    }
    page_usable_latest_.descriptor_sets_remaining = descriptor_sets_per_page_ - 1;
  }
  return descriptor_set;
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
