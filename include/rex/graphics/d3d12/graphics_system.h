/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <memory>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/graphics/graphics_system.h>

namespace rex::graphics::d3d12 {

class D3D12GraphicsSystem : public GraphicsSystem {
 public:
  D3D12GraphicsSystem();
  ~D3D12GraphicsSystem() override;

  static bool IsAvailable();

  std::string name() const override;

  X_STATUS Setup(runtime::Processor* processor, system::KernelState* kernel_state,
                 ui::WindowedAppContext* app_context, bool with_presentation) override;

 protected:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::graphics::d3d12
