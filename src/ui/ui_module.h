/**
 * @file        ui/ui_module.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/app/app_info.h>
#include <rex/logging/log_category.h>
#include <rex/module.h>

#include "ui/ui_system.h"

namespace rex::ui {

/** Serves the UI system and its control interface, and selects the drivers at Init. */
class UIModule final : public IModule {
 public:
  REX_MODULE_SERVES(::rex::ui::IUISystem, ::rex::ui::IUIControl)

  std::string_view GetName() const override { return "rexui"; }
  std::span<const InterfaceType> GetRequiredInterfaces() const override;
  [[nodiscard]] Result<void> Connect(IInterfaceFactory& factory) override;
  void Disconnect() override;
  [[nodiscard]] Result<void> Init() override;
  void Shutdown() override;
  IInterface* QueryInterface(std::string_view name, u32 version) override;

 private:
  UISystem& GetOrCreateSystem();

  const rex::AppSysRequirements* needs_ = nullptr;
  IAppInfo* info_ = nullptr;
  std::unique_ptr<UISystem> system_;
  bool retired_ = false;
  IInterfaceFactory* factory_ = nullptr;
  ISettingsRegistry* settings_ = nullptr;
  ILogger* logger_ = nullptr;
  IDriverContext* context_ = nullptr;
  std::unique_ptr<IDriver> driver_;
};

}  // namespace rex::ui
