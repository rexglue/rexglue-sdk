/**
 * @file        tests/unit/base/driver_select_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <string>
#include <thread>
#include <vector>

#include "base/driver_context.h"
#include "base/module_group.h"
#include <rex/logging/logger.h>
#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/driver_module.h>
#include <rex/settings_registry.h>

namespace {

std::vector<std::string> g_events;

template <char Name>
class NamedDriver final : public rex::IDriver {
 public:
  std::string_view GetName() const override { return name_; }
  rex::Result<void> Initialize(rex::IDriverContext&) override {
    g_events.push_back(std::string("init ") + Name);
    return {};
  }
  void Shutdown() override { g_events.push_back(std::string("shutdown ") + Name); }

 private:
  std::string name_ = std::string(1, Name);
};

class FailingDriver final : public rex::IDriver {
 public:
  std::string_view GetName() const override { return "bad"; }
  rex::Result<void> Initialize(rex::IDriverContext&) override {
    return rex::Err(rex::ErrorCategory::kModule, "no device");
  }
  void Shutdown() override {}
};

template <char Name>
std::unique_ptr<rex::IDriver> Create() {
  return std::make_unique<NamedDriver<Name>>();
}

std::unique_ptr<rex::IDriver> CreateFailing() {
  return std::make_unique<FailingDriver>();
}

class TestModule final : public rex::IModule, public rex::IDriverModule {
 public:
  std::string_view GetName() const override { return "test"; }
  rex::Result<void> Connect(rex::IInterfaceFactory&) override { return {}; }
  void Disconnect() override {}
  rex::Result<void> Init() override { return {}; }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    return rex::FindModuleInterface<rex::IDriverModule>(*this, name, version);
  }
  std::span<const rex::DriverRegistration> GetDrivers() const override { return kDrivers; }

 private:
  static constexpr std::array<rex::DriverRegistration, 3> kDrivers = {
      rex::DriverRegistration{rex::DriverType::kInput, "a", {}, &Create<'a'>},
      rex::DriverRegistration{rex::DriverType::kInput, "b", {}, &Create<'b'>},
      rex::DriverRegistration{rex::DriverType::kInput, "bad", {}, &CreateFailing}};
};

struct Fixture {
  Fixture() : context(REX_SETTINGS(), REX_LOGGER()) {
    g_events.clear();
    REQUIRE(group.Add(module));
    REQUIRE(group.Connect());
  }
  TestModule module;
  rex::ModuleGroup group;
  rex::AppDriverContext context;
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "selected drivers keep the order of the list",
                     "[base][driver_select]") {
  auto drivers = rex::SelectDrivers(group, rex::DriverType::kInput, "b, a", context);
  REQUIRE(drivers.has_value());
  REQUIRE(drivers->size() == 2);
  CHECK((*drivers)[0]->GetName() == "b");
  CHECK((*drivers)[1]->GetName() == "a");
  const std::vector<std::string> expected = {"init b", "init a"};
  CHECK(g_events == expected);
}

REX_TEST_CASE_METHOD(Fixture, "none and an empty list select no drivers", "[base][driver_select]") {
  for (std::string_view names : {"none", "", " "}) {
    auto drivers = rex::SelectDrivers(group, rex::DriverType::kInput, names, context);
    REQUIRE(drivers.has_value());
    CHECK(drivers->empty());
  }
  CHECK(g_events.empty());
}

REX_TEST_CASE_METHOD(Fixture, "a missing driver errors naming it", "[base][driver_select]") {
  auto drivers = rex::SelectDrivers(group, rex::DriverType::kInput, "a,nope", context);
  REQUIRE_FALSE(drivers.has_value());
  CHECK(drivers.error().message.find("'nope'") != std::string::npos);
  const std::vector<std::string> expected = {"init a", "shutdown a"};
  CHECK(g_events == expected);
}

REX_TEST_CASE_METHOD(Fixture, "a driver that fails to initialize errors naming it",
                     "[base][driver_select]") {
  auto drivers = rex::SelectDrivers(group, rex::DriverType::kInput, "a,bad", context);
  REQUIRE_FALSE(drivers.has_value());
  CHECK(drivers.error().message.find("'bad'") != std::string::npos);
  CHECK(drivers.error().message.find("no device") != std::string::npos);
}

REX_TEST_CASE_METHOD(Fixture, "a name no connected module provides is not loaded",
                     "[base][driver_select]") {
  auto missing = rex::SelectDrivers(group, rex::DriverType::kInput, "nope", context);
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().message.find("'nope'") != std::string::npos);
  CHECK(group.FindAll<rex::IDriverModule>().size() == 1);
}

REX_TEST_CASE("driver module names follow the driver type and the family",
              "[base][driver_select]") {
  using rex::DriverType;
  const std::vector<std::string> audio = {"rexapu-sdl3", "rexapu-b"};
  CHECK(rex::GetDriverModules(DriverType::kAudio, " sdl3, none,,sdl3 ,b") == audio);
  const std::vector<std::string> input = {"rexhid-xinput"};
  CHECK(rex::GetDriverModules(DriverType::kInput, "xinput") == input);
  const std::vector<std::string> ui = {"rexui-sdl3"};
  CHECK(rex::GetDriverModules(DriverType::kUI, "sdl3") == ui);
  const std::vector<std::string> gpu = {"rexgpu-xenos-vulkan"};
  CHECK(rex::GetDriverModules(DriverType::kGraphics, "vulkan", rex::GraphicsFamily::kXenos) == gpu);
  CHECK(rex::GetDriverModules(DriverType::kGraphics, "vulkan").empty());
  CHECK(rex::GetDriverModules(DriverType::kAudio, "none").empty());
}

REX_TEST_CASE_METHOD(Fixture, "the app context runs work only on the creating thread",
                     "[base][driver_select]") {
  bool ran = false;
  context.PostToUIThread([&] { ran = true; });
  CHECK(ran);
  ran = false;
  CHECK(context.RunOnUIThread([&] { ran = true; }).has_value());
  CHECK(ran);

  bool other_ran = false;
  bool other_accepted = true;
  std::thread other([&] {
    context.PostToUIThread([&] { other_ran = true; });
    other_accepted = context.RunOnUIThread([&] { other_ran = true; }).has_value();
  });
  other.join();
  CHECK_FALSE(other_accepted);
  CHECK_FALSE(other_ran);
}
