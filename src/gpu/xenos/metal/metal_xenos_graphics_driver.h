/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * Metal GPU backend for the current xenos driver interface. Mirrors
 * vulkan/vulkan_xenos_graphics_driver: the base class owns presentation and
 * the driver only has to build a provider and a command processor.
 */

#pragma once

#include <memory>
#include <string_view>

#include "gpu/xenos/xenos_graphics_driver_base.h"

namespace rex::gpu::metal {

/** Installs the provider factory the UI layer owns; set once before drivers are created. */
using MetalProviderFactoryFn = std::unique_ptr<::rex::gpu::GraphicsProvider> (*)();
extern MetalProviderFactoryFn MetalProviderFactory;

class MetalXenosGraphicsDriver : public XenosGraphicsDriverBase {
 public:
  MetalXenosGraphicsDriver();
  ~MetalXenosGraphicsDriver() override;

  [[nodiscard]] Result<void> Initialize(IDriverContext& context) override;
  void Shutdown() override;

  std::string_view GetName() const override { return "metal"; }

 protected:
  void CreateProvider(bool with_presentation) override;

 private:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::gpu::metal
