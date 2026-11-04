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

#include <format>
#include <rex/driver_context.h>
#include <rex/settings_registry.h>

#include "gpu/xenos/vulkan/command_processor.h"
#include "gpu/xenos/vulkan/vulkan_xenos_graphics_driver.h"
#include "gpu/xenos/host/vulkan/vulkan_provider.h"

namespace rex::gpu::vulkan {

VulkanXenosGraphicsDriver::VulkanXenosGraphicsDriver() {}

VulkanXenosGraphicsDriver::~VulkanXenosGraphicsDriver() {
  Shutdown();
}

Result<void> VulkanXenosGraphicsDriver::Initialize(IDriverContext& context) {
  if (vulkan_registry_ != nullptr) {
    return Ok();
  }
  if (auto base = XenosGraphicsDriverBase::Initialize(context); !base) {
    return base;
  }
  if (auto registered = context.GetSettingsRegistry().Register(vulkan_settings_); !registered) {
    XenosGraphicsDriverBase::Shutdown();
    return Err(ErrorCategory::kConfig,
               std::format("gpu.vulkan settings: {}", registered.error().message));
  }
  vulkan_registry_ = &context.GetSettingsRegistry();
  return Ok();
}

void VulkanXenosGraphicsDriver::Shutdown() {
  XenosGraphicsDriverBase::Shutdown();
  if (vulkan_registry_ != nullptr) {
    vulkan_registry_->Unregister(vulkan_settings_);
    vulkan_registry_ = nullptr;
  }
}

void VulkanXenosGraphicsDriver::CreateProvider(bool with_presentation) {
  provider_ = rex::gpu::vulkan::VulkanProvider::Create(
      true, with_presentation, vulkan_settings_.device.Get(), vulkan_settings_);
}

std::unique_ptr<CommandProcessor> VulkanXenosGraphicsDriver::CreateCommandProcessor() {
  return std::make_unique<VulkanCommandProcessor>(this, title_info_, *physical_memory_,
                                                  *write_watch_, *guest_interrupts_);
}

}  // namespace rex::gpu::vulkan
