/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/assert.h>
#include <rex/logging/log_category.h>
#include "gpu/xenos/host/vulkan/ui_sampler.h"
#include "gpu/xenos/host/vulkan/vulkan_objects.h"
#include "gpu/xenos/host/vulkan/vulkan_to_string.h"

namespace rex {
namespace gpu {
namespace vulkan {

std::unique_ptr<UISamplerSet> UISamplerSet::Create(const VulkanDevice* const vulkan_device) {
  REX_ASSERT(vulkan_device != nullptr);

  auto ui_samplers = std::make_unique<UISamplerSet>(Key{}, vulkan_device);

  const VulkanDevice::Functions& dfn = vulkan_device->GetFunctions();
  const VkDevice device = vulkan_device->GetDevice();

  auto sampler_create_info =
      objects::BuildStructure<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);

  for (int sampler_index = 0; sampler_index < kSamplerCount; ++sampler_index) {
    if (sampler_index == kSamplerIndexLinearRepeat ||
        sampler_index == kSamplerIndexLinearClampToEdge) {
      sampler_create_info.magFilter = VK_FILTER_LINEAR;
      sampler_create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    } else {
      sampler_create_info.magFilter = VK_FILTER_NEAREST;
      sampler_create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    }
    sampler_create_info.minFilter = sampler_create_info.magFilter;

    if (sampler_index == kSamplerIndexNearestClampToEdge ||
        sampler_index == kSamplerIndexLinearClampToEdge) {
      sampler_create_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    } else {
      sampler_create_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
    sampler_create_info.addressModeV = sampler_create_info.addressModeU;
    sampler_create_info.addressModeW = sampler_create_info.addressModeU;

    const VkResult sampler_create_result = dfn.vkCreateSampler(
        device, &sampler_create_info, nullptr, &ui_samplers->samplers_[sampler_index]);
    if (sampler_create_result != VK_SUCCESS) {
      REXUI_FAIL(
          nullptr,
          "Failed to create the Vulkan UI sampler with filter {}, addressing "
          "mode {}: {}",
          vk::to_string(static_cast<vk::Filter>(sampler_create_info.magFilter)),
          vk::to_string(static_cast<vk::SamplerAddressMode>(sampler_create_info.addressModeU)),
          vk::to_string(static_cast<vk::Result>(sampler_create_result)));
    }
  }

  return ui_samplers;
}

UISamplerSet::~UISamplerSet() {
  for (const VkSampler sampler : samplers_) {
    if (sampler == VK_NULL_HANDLE) {
      continue;
    }
    vulkan_device_->GetFunctions().vkDestroySampler(vulkan_device_->GetDevice(), sampler, nullptr);
  }
}

UISamplerSet::UISamplerSet(Key, const VulkanDevice* vulkan_device) : vulkan_device_(vulkan_device) {
  REX_ASSERT(vulkan_device != nullptr);

  samplers_.fill(VK_NULL_HANDLE);
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
