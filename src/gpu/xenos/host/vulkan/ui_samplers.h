#pragma once
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

#include <array>
#include <memory>

#include "gpu/xenos/host/vulkan/device.h"
#include <rex/types.h>

namespace rex {
namespace gpu {
namespace vulkan {

/// Samplers that can be used for presentation and UI drawing.
/// Because maxSamplerAllocationCount can be as low as 4000 (on Nvidia and Intel
/// GPUs primarily), no other samplers must be created for UI purposes.
/// The rest of the sampler allocation space on the device must be available to
/// GPU emulation.
class UISamplers : public rex::NonCopyable {
  struct Key {};

 public:
  explicit UISamplers(Key, const VulkanDevice* vulkan_device);

  static std::unique_ptr<UISamplers> Create(const VulkanDevice* vulkan_device);

  ~UISamplers();

  enum SamplerIndex {
    kSamplerIndexNearestRepeat,
    kSamplerIndexNearestClampToEdge,
    kSamplerIndexLinearRepeat,
    kSamplerIndexLinearClampToEdge,

    kSamplerCount,
  };

  const std::array<VkSampler, kSamplerCount>& samplers() const { return samplers_; }

 private:
  const VulkanDevice* vulkan_device_;

  std::array<VkSampler, kSamplerCount> samplers_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
