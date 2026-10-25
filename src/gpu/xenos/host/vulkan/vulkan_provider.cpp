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

#include <algorithm>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging/log.h>
#include "gpu/xenos/host/vulkan/immediate_drawer.h"
#include "gpu/xenos/host/vulkan/presenter.h"
#include "gpu/xenos/host/vulkan/provider.h"

#include "gpu/xenos/vulkan/cvar_table.h"

namespace rex {
namespace gpu {
namespace vulkan {

std::unique_ptr<VulkanProvider> VulkanProvider::Create(const bool with_gpu_emulation,
                                                       const bool with_presentation,
                                                       const i32 device_index) {
  auto provider = std::make_unique<VulkanProvider>(Key{});

  provider->vulkan_instance_ =
      VulkanInstance::Create(with_presentation, REXCVAR_GET(::rex::gpu::cvars::validation));
  if (!provider->vulkan_instance_) {
    return nullptr;
  }

  std::vector<VkPhysicalDevice> physical_devices;
  provider->vulkan_instance_->EnumeratePhysicalDevices(physical_devices);

  if (physical_devices.empty()) {
    REXLOG_WARN(ui, "No Vulkan physical devices available");
    return nullptr;
  }

  const VulkanInstance::Functions& ifn = provider->vulkan_instance_->GetFunctions();

  REXLOG_WARN(ui,
              "Available Vulkan physical devices (set Config::gpu.vulkan_device to force a "
              "device):");
  for (size_t physical_device_index = 0; physical_device_index < physical_devices.size();
       ++physical_device_index) {
    VkPhysicalDeviceProperties physical_device_properties;
    ifn.vkGetPhysicalDeviceProperties(physical_devices[physical_device_index],
                                      &physical_device_properties);
    REXLOG_WARN(ui, "* {}: {}", physical_device_index,
                std::string_view{physical_device_properties.deviceName});
  }

  if (device_index >= 0 && static_cast<u32>(device_index) < physical_devices.size()) {
    provider->vulkan_device_ = VulkanDevice::CreateIfSupported(
        provider->vulkan_instance_.get(), physical_devices[static_cast<size_t>(device_index)],
        with_gpu_emulation, with_presentation);
  }

  if (!provider->vulkan_device_) {
    std::vector<VkPhysicalDevice> physical_devices_ordered = physical_devices;
    bool prefer_geometry_shader = REXCVAR_GET(::rex::gpu::cvars::prefer_geometry_shader);
    bool prefer_fragment_stores =
        REXCVAR_GET(::rex::gpu::cvars::prefer_fragment_stores_and_atomics);
    bool prefer_vertex_stores =
        REXCVAR_GET(::rex::gpu::cvars::prefer_vertex_pipeline_stores_and_atomics);
    bool prefer_fill_mode_non_solid = REXCVAR_GET(::rex::gpu::cvars::prefer_fill_mode_non_solid);
    if (with_gpu_emulation && physical_devices.size() > 1 &&
        (prefer_geometry_shader || prefer_fragment_stores || prefer_vertex_stores ||
         prefer_fill_mode_non_solid)) {
      struct PhysicalDeviceScore {
        VkPhysicalDevice physical_device;
        u32 score;
      };
      std::vector<PhysicalDeviceScore> scored_devices;
      scored_devices.reserve(physical_devices.size());
      for (const VkPhysicalDevice physical_device : physical_devices) {
        VkPhysicalDeviceFeatures supported_features = {};
        ifn.vkGetPhysicalDeviceFeatures(physical_device, &supported_features);
        u32 score = 0;
        if (prefer_geometry_shader && supported_features.geometryShader) {
          ++score;
        }
        if (prefer_fragment_stores && supported_features.fragmentStoresAndAtomics) {
          ++score;
        }
        if (prefer_vertex_stores && supported_features.vertexPipelineStoresAndAtomics) {
          ++score;
        }
        if (prefer_fill_mode_non_solid && supported_features.fillModeNonSolid) {
          ++score;
        }
        scored_devices.push_back({physical_device, score});
      }

      std::stable_sort(scored_devices.begin(), scored_devices.end(),
                       [](const PhysicalDeviceScore& a, const PhysicalDeviceScore& b) {
                         return a.score > b.score;
                       });

      if (!scored_devices.empty() && scored_devices.front().score != scored_devices.back().score) {
        physical_devices_ordered.clear();
        physical_devices_ordered.reserve(scored_devices.size());
        for (const PhysicalDeviceScore& scored_device : scored_devices) {
          physical_devices_ordered.push_back(scored_device.physical_device);
        }
      }
    }

    for (const VkPhysicalDevice physical_device : physical_devices_ordered) {
      provider->vulkan_device_ = VulkanDevice::CreateIfSupported(
          provider->vulkan_instance_.get(), physical_device, with_gpu_emulation, with_presentation);
      if (provider->vulkan_device_) {
        break;
      }
    }

    if (!provider->vulkan_device_) {
      REXLOG_WARN(ui,
                  "Couldn't choose a compatible Vulkan physical device or initialize a "
                  "Vulkan logical device");
      return nullptr;
    }
  }

  if (with_presentation) {
    provider->ui_samplers_ = UISamplers::Create(provider->vulkan_device_.get());
    if (!provider->ui_samplers_) {
      return nullptr;
    }
  }

  return provider;
}

std::unique_ptr<Presenter> VulkanProvider::CreatePresenter(
    Presenter::HostGPULossCallback host_gpu_loss_callback) {
  return VulkanPresenter::Create(std::move(host_gpu_loss_callback), GetVulkanDevice(),
                                 GetUISamplers());
}

std::unique_ptr<OverlayDrawer> VulkanProvider::CreateImmediateDrawer() {
  return VulkanImmediateDrawer::Create(GetVulkanDevice(), GetUISamplers());
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
