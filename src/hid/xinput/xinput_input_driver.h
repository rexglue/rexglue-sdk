/**
 * @file        hid/xinput/xinput_input_driver.h
 * @brief       The XInput input driver and the rexhid-xinput provider (private, Windows only)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>

#include <rex/hid/input_driver.h>
#include <rex/hid/pad.h>
#include <rex/platform/dynlib.h>
#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/driver_module.h>

#include <windows.h>
#include <xinput.h>

namespace rex::hid::xinput {

/**
 * Pads through XInput. Plug changes are polled every 500 ms, and state comes from XInputGetStateEx
 * so the guide button is reported.
 */
class XInputInputDriver final : public IInputDriver {
 public:
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
  void Poll();

  platform::DynamicLibrary library_;
  decltype(&XInputGetCapabilities) get_capabilities_ = nullptr;
  decltype(&XInputGetState) get_state_ = nullptr;
  decltype(&XInputGetState) get_state_ex_ = nullptr;
  decltype(&XInputSetState) set_state_ = nullptr;
  std::array<std::atomic<bool>, 4> connected_{};
  std::atomic<u64> revision_{1};
  std::mutex wake_mutex_;
  std::condition_variable_any wake_;
  std::jthread plug_poll_;
};

class XInputInputProvider final : public IModule, public IDriverModule {
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

}  // namespace rex::hid::xinput
