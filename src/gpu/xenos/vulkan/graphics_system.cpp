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

#include "gpu/xenos/vulkan/command_processor.h"
#include "gpu/xenos/vulkan/graphics_system.h"
#include <rex/runtime.h>
#include "gpu/xenos/host/vulkan/provider.h"

namespace rex::gpu::vulkan {

VulkanGraphicsSystem::VulkanGraphicsSystem() {}

VulkanGraphicsSystem::~VulkanGraphicsSystem() {}

std::string VulkanGraphicsSystem::name() const {
  return "Vulkan";
}

void VulkanGraphicsSystem::CreateProvider(bool with_presentation) {
  provider_ = rex::gpu::vulkan::VulkanProvider::Create(true, with_presentation,
                                                       config_ ? config_->vulkan_device : -1);
}

std::unique_ptr<CommandProcessor> VulkanGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<VulkanCommandProcessor>(this, *runtime_);
}

}  // namespace rex::gpu::vulkan
