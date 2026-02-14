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

#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/vulkan/provider.h>

namespace rex::graphics::vulkan {

VulkanGraphicsSystem::VulkanGraphicsSystem() {}

VulkanGraphicsSystem::~VulkanGraphicsSystem() {}

std::string VulkanGraphicsSystem::name() const {
  return "Vulkan";
}

X_STATUS VulkanGraphicsSystem::Setup(runtime::Processor* processor,
                                     system::KernelState* kernel_state,
                                     ui::WindowedAppContext* app_context, bool with_presentation) {
  provider_ = rex::ui::vulkan::VulkanProvider::Create(true, with_presentation);
  return GraphicsSystem::Setup(processor, kernel_state, app_context, with_presentation);
}

std::unique_ptr<CommandProcessor> VulkanGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<VulkanCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::vulkan
