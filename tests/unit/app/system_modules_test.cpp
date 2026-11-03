/**
 * @file        tests/unit/app/system_modules_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "object_app.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <rex/app/app.h>
#include <rex/apu/audio_driver.h>
#include <rex/apu/audio_system.h>
#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/driver_module.h>
#include <rex/app/app_info.h>
#include <rex/cli.h>
#include <rex/hid/input_driver.h>
#include <rex/hid/pad.h>
#include <rex/hid/input_system.h>

#include "../ui/ui_harness.h"

namespace {

std::vector<std::string> g_events;

class OrderInput final : public rex::hid::IInputDriver {
 public:
  std::string_view GetName() const override { return "order_input"; }
  rex::Result<void> Initialize(rex::IDriverContext&) override {
    g_events.emplace_back("init input");
    return {};
  }
  void Shutdown() override { g_events.emplace_back("shutdown input"); }
  rex::u64 GetDeviceRevision() const override { return 0; }
  void EnumerateDevices(std::vector<rex::hid::PadDevice>& out) override { out.clear(); }
  rex::Result<rex::hid::PadState> GetDeviceState(rex::hid::PadDeviceID) override {
    return rex::Err<rex::hid::PadState>(rex::ErrorCategory::kNotFound, "none");
  }
  rex::Result<rex::hid::PadCapabilities> GetDeviceCapabilities(rex::hid::PadDeviceID) override {
    return rex::hid::PadCapabilities{};
  }
  rex::Result<void> SetDeviceVibration(rex::hid::PadDeviceID,
                                       const rex::hid::PadVibration&) override {
    return {};
  }
};

class OrderAudio final : public rex::apu::IAudioDriver {
 public:
  std::string_view GetName() const override { return "order_audio"; }
  rex::Result<void> Initialize(rex::IDriverContext&) override {
    g_events.emplace_back("init audio");
    return {};
  }
  void Shutdown() override { g_events.emplace_back("shutdown audio"); }
  rex::Result<rex::u32> OpenDevice(rex::u32 wanted, rex::apu::AudioFill) override { return wanted; }
  void CloseDevice() override {}
  bool IsDeviceLost() const override { return false; }
};

class OrderUI final : public rex::test_ui::FakeUIDriver {
 public:
  std::string_view GetName() const override { return "order_ui"; }
  rex::Result<void> Initialize(rex::IDriverContext&) override {
    g_events.emplace_back("init ui");
    return {};
  }
  void Shutdown() override { g_events.emplace_back("shutdown ui"); }
};

std::unique_ptr<rex::IDriver> CreateInput() {
  return std::make_unique<OrderInput>();
}

std::unique_ptr<rex::IDriver> CreateAudio() {
  return std::make_unique<OrderAudio>();
}

std::unique_ptr<rex::IDriver> CreateUI() {
  return std::make_unique<OrderUI>();
}

class OrderProvider final : public rex::IModule, public rex::IDriverModule {
 public:
  std::string_view GetName() const override { return "order"; }
  rex::Result<void> Connect(rex::IFactory&) override { return {}; }
  void Disconnect() override {}
  rex::Result<void> Init() override { return {}; }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    return rex::FindModuleInterface<rex::IDriverModule>(*this, name, version);
  }
  std::span<const rex::DriverRegistration> GetDrivers() const override { return kDrivers; }

 private:
  static constexpr std::array<rex::DriverRegistration, 3> kDrivers = {
      rex::DriverRegistration{rex::DriverType::kInput, "order_input", {}, &CreateInput},
      rex::DriverRegistration{rex::DriverType::kAudio, "order_audio", {}, &CreateAudio},
      rex::DriverRegistration{rex::DriverType::kUI, "order_ui", {}, &CreateUI}};
};

class OrderApp final : public rex::test::ObjectApp<rex::app::App> {
 public:
  OrderApp() : ObjectApp("order_test") {
    static_cast<rex::test_ui::FakeUIData&>(rex::test_ui::g_ui) = rex::test_ui::FakeUIData{};
  }

  bool needs_ui = false;
  bool dirty = false;

  rex::Result<void> Start() {
    auto started = Init(rex::CLIArgList::Parse(std::vector<std::string>{}));
    if (started && dirty) {
      auto stream = GetAudio().OpenStream(rex::MoveOnlyFunction<void()>([] {}));
      if (stream) {
        stream_ = *stream;
      }
    }
    return started;
  }

  int Finish() { return Shutdown(); }

  rex::AppSysRequirements GetRequirements() const override {
    rex::AppSysRequirements needs;
    needs.ui = needs_ui;
    return needs;
  }

 protected:
  void OnRegisterModules(std::vector<rex::IModule*>& providers) override {
    ObjectApp::OnRegisterModules(providers);
    providers.push_back(&provider_);
  }

  rex::Result<void> OnConfigure(rex::Settings&) override {
    (void)root.SetText(rex::SettingLayer::kTitle, folder_.path.string());
    (void)GetInput().GetSettings().drivers.SetText(rex::SettingLayer::kTitle, "order_input");
    (void)GetAudio().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "order_audio");
    if (dirty) {
      (void)GetAudio().GetSettings().mute.SetText(rex::SettingLayer::kTitle, "true");
    }
    (void)GetUI().GetSettings().driver.SetText(rex::SettingLayer::kTitle, "order_ui");
    return rex::Ok();
  }

 private:
  rex::test::TempFolder folder_{"rex_order_"};
  rex::apu::AudioStreamID stream_{};
  OrderProvider provider_;
};

}  // namespace

REX_TEST_CASE("systems stop in reverse start order", "[app][app][modules]") {
  g_events.clear();
  OrderApp app;
  REQUIRE(app.Start());
  app.Finish();
  const std::vector<std::string> expected = {"init input",  "init audio",     "init ui",
                                             "shutdown ui", "shutdown audio", "shutdown input"};
  CHECK(g_events == expected);
}

REX_TEST_CASE("a system that fails to init is stopped once and the earlier ones are stopped",
              "[app][app][modules]") {
  g_events.clear();
  OrderApp app;
  app.needs_ui = true;
  rex::test_ui::g_ui.open_fails = true;
  CHECK_FALSE(app.Start());
  app.Finish();
  const std::vector<std::string> expected = {"init input",  "init audio",     "init ui",
                                             "shutdown ui", "shutdown audio", "shutdown input"};
  CHECK(g_events == expected);
}

REX_TEST_CASE("a second app in one process starts from default systems", "[app][app][modules]") {
  {
    OrderApp first;
    first.dirty = true;
    REQUIRE(first.Start());
    CHECK(first.GetAudio().GetSettings().mute.Get());
    CHECK(first.GetAudio().GetStats().open_streams == 1);
    first.Finish();
  }
  OrderApp second;
  REQUIRE(second.Start());
  CHECK_FALSE(second.GetAudio().GetSettings().mute.Get());
  CHECK(second.GetAudio().GetStats().open_streams == 0);
  second.Finish();
}
