#pragma once
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

#include <memory>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>

namespace rex::graphics::vulkan {

class VulkanGraphicsSystem : public GraphicsSystem {
 public:
  VulkanGraphicsSystem();
  ~VulkanGraphicsSystem() override;

  static bool IsAvailable() { return true; }

  std::string name() const override;

  X_STATUS Setup(runtime::Processor* processor, system::KernelState* kernel_state,
                 ui::WindowedAppContext* app_context, bool with_presentation) override;

 private:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::graphics::vulkan
