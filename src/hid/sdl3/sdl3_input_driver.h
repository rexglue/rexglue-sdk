/**
 * @file        hid/sdl3/sdl3_input_driver.h
 * @brief       The SDL3 input driver and the rexhid-sdl3 provider (private to the module)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

#include <rex/hid/input_driver.h>
#include <rex/hid/pad.h>
#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/driver_module.h>
#include <rex/setting.h>

namespace rex::hid::sdl3 {

/**
 * Pads through the SDL3 gamepad API. On Windows PadIdentity::xinput_slot is SDL's player index,
 * which SDL sets to the XInput slot for XInput pads.
 */
class SDL3InputDriver final : public IInputDriver, public SettingOwner {
 public:
  REX_SETTING(std::filesystem::path, mappings_file, "{app.root}/gamecontrollerdb.txt",
              "SDL gamepad mappings file", kStartupOnly);

  SDL3InputDriver();

  std::string_view GetName() const override;
  [[nodiscard]] Result<void> Initialize(IDriverContext& context) override;
  void Shutdown() override;

  u64 GetDeviceRevision() const override;
  void EnumerateDevices(std::vector<PadDevice>& out) override;
  [[nodiscard]] Result<PadState> GetDeviceState(PadDeviceID id) override;
  [[nodiscard]] Result<PadCapabilities> GetDeviceCapabilities(PadDeviceID id) override;
  [[nodiscard]] Result<void> SetDeviceVibration(PadDeviceID id,
                                                const PadVibration& vibration) override;

 private:
  struct State;
  std::shared_ptr<State> state_;
};

class SDL3InputProvider final : public IModule, public IDriverModule {
 public:
  REX_MODULE_SERVES(::rex::IDriverModule)

  std::string_view GetName() const override;
  [[nodiscard]] Result<void> Connect(IFactory& factory) override;
  void Disconnect() override;
  [[nodiscard]] Result<void> Init() override;
  void Shutdown() override;
  IInterface* QueryInterface(std::string_view name, u32 version) override {
    return FindModuleInterface<IDriverModule>(*this, name, version);
  }
  std::span<const DriverRegistration> GetDrivers() const override;
};

}  // namespace rex::hid::sdl3
