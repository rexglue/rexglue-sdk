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

#include <memory>

#include "gpu/xenos/host/graphics_provider.h"
#include "gpu/xenos/host/vulkan/device.h"
#include "gpu/xenos/host/vulkan/instance.h"
#include "gpu/xenos/host/vulkan/ui_samplers.h"

namespace rex {
namespace gpu {
namespace vulkan {

class VulkanProvider : public GraphicsProvider {
  struct Key {};

 public:
  explicit VulkanProvider(Key) {}

  static std::unique_ptr<VulkanProvider> Create(bool with_gpu_emulation, bool with_presentation,
                                                i32 device_index);

  VulkanInstance* GetVulkanInstance() const { return vulkan_instance_.get(); }

  VulkanDevice* GetVulkanDevice() const { return vulkan_device_.get(); }

  /**
   * Returns the UI samplers.
   *
   * @return The samplers, or null when created without presentation support.
   */
  const UISamplers* GetUISamplers() const { return ui_samplers_.get(); }

  std::unique_ptr<Presenter> CreatePresenter(Presenter::HostGPULossCallback host_gpu_loss_callback =
                                                 Presenter::FatalErrorHostGPULossCallback) override;

  std::unique_ptr<OverlayDrawer> CreateImmediateDrawer() override;

 private:
  std::unique_ptr<VulkanInstance> vulkan_instance_;

  // Depends on the instance.
  std::unique_ptr<VulkanDevice> vulkan_device_;

  // Depends on the device.
  std::unique_ptr<UISamplers> ui_samplers_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
