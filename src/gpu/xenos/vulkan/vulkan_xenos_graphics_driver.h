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

#pragma once

#include <memory>
#include <rex/settings_registry.h>

#include "gpu/xenos/command_processor.h"
#include "gpu/xenos/host/vulkan/vulkan_settings.h"
#include "gpu/xenos/xenos_graphics_driver_base.h"

namespace rex::gpu::vulkan {

class VulkanXenosGraphicsDriver : public XenosGraphicsDriverBase {
 public:
  VulkanXenosGraphicsDriver();
  ~VulkanXenosGraphicsDriver() override;

  [[nodiscard]] Result<void> Initialize(IDriverContext& context) override;
  void Shutdown() override;

  const VulkanSettings& GetVulkanSettings() const { return vulkan_settings_; }

  std::string_view GetName() const override { return "vulkan"; }

 protected:
  void CreateProvider(bool with_presentation) override;

 private:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;

  VulkanSettings vulkan_settings_;
  ISettingsRegistry* vulkan_registry_ = nullptr;
};

}  // namespace rex::gpu::vulkan
