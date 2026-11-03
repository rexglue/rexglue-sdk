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
#include <rex/settings.h>

#include "gpu/xenos/command_processor.h"
#include "gpu/xenos/d3d12/deferred_command_list.h"
#include "gpu/xenos/host/d3d12/settings.h"
#include "gpu/xenos/xenos_graphics_driver_base.h"

namespace rex::gpu::d3d12 {

class D3D12XenosGraphicsDriver : public XenosGraphicsDriverBase {
 public:
  D3D12XenosGraphicsDriver();
  ~D3D12XenosGraphicsDriver() override;

  [[nodiscard]] Result<void> Initialize(IDriverContext& context) override;
  void Shutdown() override;

  const D3D12Settings& GetD3D12Settings() const { return d3d12_settings_; }

  std::string_view GetName() const override { return "d3d12"; }

 protected:
  void CreateProvider(bool with_presentation) override;
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;

  D3D12Settings d3d12_settings_;
  ISettings* d3d12_registry_ = nullptr;
};

}  // namespace rex::gpu::d3d12
