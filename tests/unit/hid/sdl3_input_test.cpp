/**
 * @file        tests/unit/hid/sdl3_input_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <vector>

#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/hid/input_driver.h>
#include <rex/logging/logger.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/utility.h>

#include "base/driver_context.h"
#include "base/module_group.h"
#include "base/module_registration.h"

namespace {

class TrackingContext final : public rex::IDriverContext {
 public:
  rex::ISettingsRegistry& GetSettingsRegistry() override { return REX_SETTINGS(); }
  rex::ILogger& GetLogger() override { return REX_LOGGER(); }
  rex::Result<void> RunOnUIThread(rex::MoveOnlyFunction<void()> fn) override {
    fn();
    return rex::Ok();
  }
  void PostToUIThread(rex::MoveOnlyFunction<void()> fn) override {
    posted.push_back(std::move(fn));
  }

  void RunPosted() {
    std::vector<rex::MoveOnlyFunction<void()>> run;
    run.swap(posted);
    for (auto& fn : run) {
      fn();
    }
  }

  std::vector<rex::MoveOnlyFunction<void()>> posted;
};

// Takes the UI thread work and never runs it, so the driver starts no SDL.
class HeldContext final : public rex::IDriverContext {
 public:
  rex::ISettingsRegistry& GetSettingsRegistry() override { return REX_SETTINGS(); }
  rex::ILogger& GetLogger() override { return REX_LOGGER(); }
  rex::Result<void> RunOnUIThread(rex::MoveOnlyFunction<void()>) override {
    ++held;
    return rex::Ok();
  }
  void PostToUIThread(rex::MoveOnlyFunction<void()>) override { ++held; }

  int held = 0;
};

struct Fixture {
  Fixture() {
    REQUIRE(group.Load("rexhid-sdl3").has_value());
    REQUIRE(group.Connect().has_value());
  }

  std::unique_ptr<rex::hid::IInputDriver> Create() {
    for (const rex::DriverRegistration* registration :
         rex::test::GetRegistrations(group, rex::DriverType::kInput)) {
      if (registration->name == "sdl3") {
        std::unique_ptr<rex::IDriver> driver = registration->create();
        return std::unique_ptr<rex::hid::IInputDriver>(
            static_cast<rex::hid::IInputDriver*>(driver.release()));
      }
    }
    return nullptr;
  }

  rex::ModuleGroup group;
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "sdl3 devices are live after Initialize", "[hid][sdl3]") {
  auto driver = Create();
  REQUIRE(driver);
  TrackingContext context;
  const u64 before = driver->GetDeviceRevision();
  REQUIRE(driver->Initialize(context).has_value());
  CHECK(driver->GetDeviceRevision() > before);
  // With hid.sdl.fresh_read on (the default), the read first waits for a refresh round on the
  // pad thread, then reports the unknown pad as not connected.
  CHECK(!driver->GetDeviceState(rex::hid::PadDeviceID{999}).has_value());

  driver->Shutdown();
  context.RunPosted();
}

REX_TEST_CASE_METHOD(Fixture,
                     "mappings file setting is hid.sdl.mappings_file with alias from "
                     "hid.mappings_file",
                     "[hid][sdl3]") {
  auto driver = Create();
  REQUIRE(driver);
  rex::AppDriverContext context(REX_SETTINGS(), REX_LOGGER());
  REQUIRE(driver->Initialize(context).has_value());

  auto found = REX_SETTINGS().Find("hid.sdl.mappings_file");
  REQUIRE(found.has_value());
  REQUIRE(REX_SETTINGS()
              .SetText(rex::SettingLayer::kTitle, "hid.mappings_file", "pads.txt")
              .has_value());
  CHECK((*found)->GetText() == "pads.txt");

  driver->Shutdown();
}

REX_TEST_CASE_METHOD(Fixture,
                     "fresh read setting is hid.sdl.fresh_read, on by default, applied at restart",
                     "[hid][sdl3]") {
  auto driver = Create();
  REQUIRE(driver);
  HeldContext context;
  REQUIRE(driver->Initialize(context).has_value());
  CHECK(context.held == 1);

  auto found = REX_SETTINGS().Find("hid.sdl.fresh_read");
  REQUIRE(found.has_value());
  CHECK((*found)->GetType() == rex::SettingType::kBool);
  CHECK((*found)->GetDefaultText() == "true");
  CHECK((*found)->GetText() == "true");
  CHECK((*found)->GetConstraints().mode == rex::SettingMode::kRequiresRestart);

  driver->Shutdown();
  CHECK(!REX_SETTINGS().Find("hid.sdl.fresh_read").has_value());
}
