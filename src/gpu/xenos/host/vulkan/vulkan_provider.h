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
#include <rex/settings_registry.h>

#include "gpu/xenos/host/graphics_provider.h"
#include "gpu/xenos/host/vulkan/vulkan_device.h"
#include "gpu/xenos/host/vulkan/vulkan_instance.h"
#include "gpu/xenos/host/vulkan/vulkan_settings.h"
#include "gpu/xenos/host/vulkan/ui_sampler.h"

namespace rex {
namespace gpu {
namespace vulkan {

class VulkanProvider : public GraphicsProvider {
  struct Key {};

 public:
  explicit VulkanProvider(Key) {}

  /**
   * @param with_gpu_emulation True to require the device features Xenos emulation needs.
   * @param with_presentation True to create the instance with surface support.
   * @param device_index The physical device to use, or -1 to choose.
   * @param settings The gpu.vulkan settings, which must outlive the provider.
   * @return The provider, or null when no usable device exists.
   */
  static std::unique_ptr<VulkanProvider> Create(bool with_gpu_emulation, bool with_presentation,
                                                i32 device_index, const VulkanSettings& settings);

  const VulkanSettings& GetSettings() const { return *settings_; }

  VulkanInstance* GetVulkanInstance() const { return vulkan_instance_.get(); }

  VulkanDevice* GetVulkanDevice() const { return vulkan_device_.get(); }

  /** Returns the UI samplers, or null when created without presentation support. */
  const UISamplerSet* GetUISamplers() const { return ui_samplers_.get(); }

  std::unique_ptr<Presenter> CreatePresenter(const XenosGraphicsDriverBase& driver,
                                             Presenter::HostGPULossCallback host_gpu_loss_callback =
                                                 Presenter::FatalErrorHostGPULossCallback) override;

  std::unique_ptr<OverlayDrawer> CreateImmediateDrawer() override;

 private:
  std::unique_ptr<VulkanInstance> vulkan_instance_;

  // Depends on the instance.
  std::unique_ptr<VulkanDevice> vulkan_device_;
  const VulkanSettings* settings_ = nullptr;

  // Depends on the device.
  std::unique_ptr<UISamplerSet> ui_samplers_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
