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
#include <format>

#include "gpu/xenos/d3d12/command_processor.h"
#include "gpu/xenos/d3d12/d3d12_xenos_graphics_driver.h"
#include "gpu/xenos/draw_state.h"
#include <rex/logging/log_category.h>
#include <rex/math.h>
#include <rex/driver_context.h>
#include <rex/settings_registry.h>
#include "gpu/xenos/host/d3d12/d3d12_objects.h"

namespace rex::gpu::d3d12 {

D3D12XenosGraphicsDriver::D3D12XenosGraphicsDriver() {}

D3D12XenosGraphicsDriver::~D3D12XenosGraphicsDriver() {
  Shutdown();
}

Result<void> D3D12XenosGraphicsDriver::Initialize(IDriverContext& context) {
  if (d3d12_registry_ != nullptr) {
    return Ok();
  }
  if (auto base = XenosGraphicsDriverBase::Initialize(context); !base) {
    return base;
  }
  if (auto registered = context.GetSettingsRegistry().Register(d3d12_settings_); !registered) {
    XenosGraphicsDriverBase::Shutdown();
    return Err(ErrorCategory::kConfig,
               std::format("gpu.d3d12 settings: {}", registered.error().message));
  }
  d3d12_registry_ = &context.GetSettingsRegistry();
  return Ok();
}

void D3D12XenosGraphicsDriver::Shutdown() {
  XenosGraphicsDriverBase::Shutdown();
  if (d3d12_registry_ != nullptr) {
    d3d12_registry_->Unregister(d3d12_settings_);
    d3d12_registry_ = nullptr;
  }
}

void D3D12XenosGraphicsDriver::CreateProvider(bool /*with_presentation*/) {
  // The presenter creates swapchains per window, so the provider ignores with_presentation.
  provider_ =
      rex::gpu::d3d12::D3D12Provider::Create(d3d12_settings_.adapter.Get(), d3d12_settings_);
}

std::unique_ptr<CommandProcessor> D3D12XenosGraphicsDriver::CreateCommandProcessor() {
  return std::make_unique<D3D12CommandProcessor>(this, title_info_, *physical_memory_,
                                                 *write_watch_, *guest_interrupts_);
}

}  // namespace rex::gpu::d3d12
