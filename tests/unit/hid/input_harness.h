/**
 * @file        tests/unit/hid/input_harness.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "rex_test.h"
#include "object_app.h"

#include <array>
#include <exception>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <rex/app/app.h>
#include <rex/hid/input_driver.h>
#include <rex/hid/pad.h>
#include <rex/hid/input_system.h>
#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/driver_module.h>
#include <rex/ui/ui_system.h>

#include "temp_folder.h"

namespace rex::test_input {

using rex::hid::PadDevice;
using rex::hid::PadDeviceID;
using rex::hid::PadState;

struct FakeState {
  std::vector<rex::hid::PadDevice> devices;
  std::map<u64, rex::hid::PadState> states;
  u64 revision = 1;
  std::vector<rex::hid::PadVibration> vibrations;
};

inline std::array<FakeState, 2> g_fake;

inline rex::hid::PadDevice MakePad(u64 id, i32 xinput_slot, const char* name = "pad") {
  rex::hid::PadDevice device;
  device.id = static_cast<rex::hid::PadDeviceID>(id);
  device.name = name;
  device.identity.xinput_slot = xinput_slot;
  return device;
}

template <int N>
class FakeDriver final : public rex::hid::IInputDriver {
 public:
  std::string_view GetName() const override { return N == 0 ? "fake_a" : "fake_b"; }
  rex::Result<void> Initialize(rex::IDriverContext&) override { return {}; }
  void Shutdown() override {}
  u64 GetDeviceRevision() const override { return g_fake[N].revision; }
  void EnumerateDevices(std::vector<rex::hid::PadDevice>& out) override { out = g_fake[N].devices; }
  rex::Result<rex::hid::PadState> GetDeviceState(rex::hid::PadDeviceID id) override {
    auto found = g_fake[N].states.find(static_cast<u64>(id));
    if (found == g_fake[N].states.end()) {
      return rex::Err<rex::hid::PadState>(rex::ErrorCategory::kNotFound, "gone");
    }
    return found->second;
  }
  rex::Result<rex::hid::PadCapabilities> GetDeviceCapabilities(rex::hid::PadDeviceID) override {
    rex::hid::PadCapabilities caps;
    caps.supported.buttons = 0xFFFF;
    return caps;
  }
  rex::Result<void> SetDeviceVibration(rex::hid::PadDeviceID,
                                       const rex::hid::PadVibration& vibration) override {
    g_fake[N].vibrations.push_back(vibration);
    return {};
  }
};

template <int N>
std::unique_ptr<rex::IDriver> Create() {
  return std::make_unique<FakeDriver<N>>();
}

class FakeProvider final : public rex::IModule, public rex::IDriverModule {
 public:
  std::string_view GetName() const override { return "fake"; }
  rex::Result<void> Connect(rex::IFactory&) override { return {}; }
  void Disconnect() override {}
  rex::Result<void> Init() override { return {}; }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    return rex::FindModuleInterface<rex::IDriverModule>(*this, name, version);
  }
  std::span<const rex::DriverRegistration> GetDrivers() const override { return kDrivers; }

 private:
  static constexpr std::array<rex::DriverRegistration, 2> kDrivers = {
      rex::DriverRegistration{rex::DriverType::kInput, "fake_a", {}, &Create<0>},
      rex::DriverRegistration{rex::DriverType::kInput, "fake_b", {}, &Create<1>}};
};

class InputApp final : public rex::test::ObjectApp<rex::app::App> {
 public:
  explicit InputApp(std::string names) : ObjectApp("input_test"), names_(std::move(names)) {}

  std::function<void(rex::hid::IInputSystem&)> body;
  std::exception_ptr failure;

  rex::Result<void> Start() { return Init(rex::CLIArgList::Parse(std::vector<std::string>{})); }

  int Finish() {
    const int code = Shutdown();
    if (failure) {
      std::rethrow_exception(failure);
    }
    return code;
  }

 protected:
  void OnRegisterModules(std::vector<rex::IModule*>& providers) override {
    ObjectApp::OnRegisterModules(providers);
    providers.push_back(&provider_);
  }

  rex::Result<void> OnConfigure(rex::Settings&) override {
    (void)root.SetText(rex::SettingLayer::kTitle, folder_.path.string());
    (void)GetInput().GetSettings().drivers.SetText(rex::SettingLayer::kTitle, names_);
    (void)GetUI().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "none");
    return rex::Ok();
  }

  rex::Result<void> OnInit() override {
    try {
      if (body) {
        body(GetInput());
      }
    } catch (...) {
      failure = std::current_exception();
      return rex::Err(rex::ErrorCategory::kRuntime, "the test body threw");
    }
    return rex::Ok();
  }

 private:
  std::string names_;
  rex::test::TempFolder folder_{"rex_input_"};
  FakeProvider provider_;
};

inline void Run(const std::string& names, std::function<void(rex::hid::IInputSystem&)> body) {
  g_fake = {};
  InputApp app(names);
  app.body = std::move(body);
  const bool started = static_cast<bool>(app.Start());
  app.Finish();
  REQUIRE(started);
}

inline rex::hid::PadState Buttons(u16 buttons) {
  rex::hid::PadState state;
  state.buttons = buttons;
  return state;
}

}  // namespace rex::test_input
