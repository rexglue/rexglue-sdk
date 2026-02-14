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

#include <algorithm>

#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/graphics_system.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/d3d12/d3d12_util.h>

namespace rex::graphics::d3d12 {

D3D12GraphicsSystem::D3D12GraphicsSystem() {}

D3D12GraphicsSystem::~D3D12GraphicsSystem() {}

bool D3D12GraphicsSystem::IsAvailable() {
  return rex::ui::d3d12::D3D12Provider::IsD3D12APIAvailable();
}

std::string D3D12GraphicsSystem::name() const {
  auto d3d12_command_processor = static_cast<D3D12CommandProcessor*>(command_processor());
  if (d3d12_command_processor != nullptr) {
    return d3d12_command_processor->GetWindowTitleText();
  }
  return "Direct3D 12";
}

X_STATUS D3D12GraphicsSystem::Setup(runtime::Processor* processor,
                                    system::KernelState* kernel_state,
                                    ui::WindowedAppContext* app_context, bool with_presentation) {
  provider_ = rex::ui::d3d12::D3D12Provider::Create();
  return GraphicsSystem::Setup(processor, kernel_state, app_context, with_presentation);
}

std::unique_ptr<CommandProcessor> D3D12GraphicsSystem::CreateCommandProcessor() {
  return std::unique_ptr<CommandProcessor>(new D3D12CommandProcessor(this, kernel_state_));
}

}  // namespace rex::graphics::d3d12
