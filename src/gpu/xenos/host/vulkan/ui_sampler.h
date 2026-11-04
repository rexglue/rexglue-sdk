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

#pragma once

#include <array>
#include <memory>

#include "gpu/xenos/host/vulkan/vulkan_device.h"
#include <rex/types.h>
#include <rex/utility.h>

namespace rex {
namespace gpu {
namespace vulkan {

/**
 * Samplers for presentation and UI drawing.
 *
 * maxSamplerAllocationCount can be as low as 4000, mainly on Nvidia and Intel GPUs, so the UI
 * creates no other samplers and leaves the rest of the budget to GPU emulation.
 */
class UISamplerSet : public rex::NonCopyable {
  struct Key {};

 public:
  explicit UISamplerSet(Key, const VulkanDevice* vulkan_device);

  static std::unique_ptr<UISamplerSet> Create(const VulkanDevice* vulkan_device);

  ~UISamplerSet();

  enum SamplerIndex : u8 {
    kSamplerIndexNearestRepeat,
    kSamplerIndexNearestClampToEdge,
    kSamplerIndexLinearRepeat,
    kSamplerIndexLinearClampToEdge,

    kSamplerCount,
  };

  const std::array<VkSampler, kSamplerCount>& GetSamplers() const { return samplers_; }

 private:
  const VulkanDevice* vulkan_device_;

  std::array<VkSampler, kSamplerCount> samplers_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
