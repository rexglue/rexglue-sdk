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

#include "gpu/xenos/d3d12/command_processor.h"
#include "gpu/xenos/d3d12/graphics_system.h"
#include "gpu/xenos/draw_state.h"
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/runtime.h>
#include "gpu/xenos/host/d3d12/d3d12_objects.h"

namespace rex::gpu::d3d12 {

D3D12GraphicsSystem::D3D12GraphicsSystem() {}

D3D12GraphicsSystem::~D3D12GraphicsSystem() {}

bool D3D12GraphicsSystem::IsAvailable() {
  return rex::gpu::d3d12::D3D12Provider::IsD3D12APIAvailable();
}

std::string D3D12GraphicsSystem::GetName() const {
  auto d3d12_command_processor = static_cast<D3D12CommandProcessor*>(GetCommandProcessor());
  if (d3d12_command_processor != nullptr) {
    return d3d12_command_processor->GetWindowTitleText();
  }
  return "Direct3D 12";
}

void D3D12GraphicsSystem::CreateProvider(bool /*with_presentation*/) {
  // The presenter creates swapchains per window, so the provider ignores with_presentation.
  provider_ = rex::gpu::d3d12::D3D12Provider::Create(config_ ? config_->d3d12_adapter : -1);
}

std::unique_ptr<CommandProcessor> D3D12GraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<D3D12CommandProcessor>(this, *runtime_);
}

}  // namespace rex::gpu::d3d12
