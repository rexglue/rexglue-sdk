/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Metal backend for macOS (ReXGlue runtime)
 */

#include <rex/graphics/metal/graphics_system.h>
#include <rex/graphics/metal/command_processor.h>
#include <rex/ui/metal/metal_provider.h>
#include <rex/kernel.h>
#include <rex/logging.h>
#include <rex/xenia_logging_compat.h>

namespace rex::graphics::metal {

MetalGraphicsSystem::MetalGraphicsSystem() {}

MetalGraphicsSystem::~MetalGraphicsSystem() {}

bool MetalGraphicsSystem::IsAvailable() {
  return ui::metal::MetalProvider::IsAvailable();
}

std::string MetalGraphicsSystem::name() const {
  auto metal_command_processor =
      static_cast<MetalCommandProcessor*>(command_processor());
  if (metal_command_processor != nullptr) {
    return metal_command_processor->GetWindowTitleText();
  }
  return "Metal";
}

X_STATUS MetalGraphicsSystem::Setup(runtime::Processor* processor,
                                    system::KernelState* kernel_state,
                                    ui::WindowedAppContext* app_context,
                                    bool with_presentation) {
  XELOGI("MetalGraphicsSystem: Setting up Metal graphics system");

  // Create the Metal provider.
  provider_ = ui::metal::MetalProvider::Create(true, with_presentation);
  if (!provider_) {
    XELOGE("MetalGraphicsSystem: Failed to create Metal provider");
    return X_STATUS_UNSUCCESSFUL;
  }

  return GraphicsSystem::Setup(processor, kernel_state, app_context,
                               with_presentation);
}

std::unique_ptr<CommandProcessor>
MetalGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<MetalCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::metal
