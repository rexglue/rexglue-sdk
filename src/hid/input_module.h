/**
 * @file        hid/input_module.h
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
#include <rex/app_info.h>
#include <rex/logging/log_category.h>
#include <rex/module.h>

#include "hid/input_system.h"

namespace rex::hid {

/**
 * Serves the system and its control interface, selects the drivers at Init and stops them at
 * Shutdown.
 */
class InputModule final : public IModule {
 public:
  REX_MODULE_SERVES(::rex::hid::IInputSystem)

  std::string_view GetName() const override { return "rexhid"; }
  std::span<const InterfaceType> GetRequiredInterfaces() const override;
  [[nodiscard]] Result<void> Connect(IInterfaceFactory& factory) override;
  void Disconnect() override;
  [[nodiscard]] Result<void> Init() override;
  void Shutdown() override;
  IInterface* QueryInterface(std::string_view name, u32 version) override;

 private:
  InputSystem& GetOrCreateSystem();

  const rex::AppSysRequirements* needs_ = nullptr;
  std::unique_ptr<InputSystem> system_;
  bool retired_ = false;
  IInterfaceFactory* factory_ = nullptr;
  ISettingsRegistry* settings_ = nullptr;
  ILogger* logger_ = nullptr;
  IDriverContext* context_ = nullptr;
  std::vector<std::unique_ptr<IDriver>> drivers_;
};

}  // namespace rex::hid
