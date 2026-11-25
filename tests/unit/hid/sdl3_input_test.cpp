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

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_joystick.h>

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

REX_TEST_CASE_METHOD(Fixture, "sdl3 a pad read returns the state of the poll it makes",
                     "[hid][sdl3]") {
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  auto driver = Create();
  REQUIRE(driver);
  TrackingContext context;
  REQUIRE(driver->Initialize(context).has_value());

  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
  desc.button_mask = 0x7FFF;
  desc.axis_mask = 0x3F;
  desc.name = "virtual pad";
  const SDL_JoystickID instance = SDL_AttachVirtualJoystick(&desc);
  REQUIRE(instance != 0);
  SDL_Joystick* joystick = SDL_OpenJoystick(instance);
  REQUIRE(joystick != nullptr);

  std::vector<rex::hid::PadDevice> devices;
  for (int attempt = 0; attempt < 10 && devices.empty(); ++attempt) {
    context.RunPosted();
    driver->EnumerateDevices(devices);
  }
  REQUIRE(devices.size() == 1);
  const rex::hid::PadDeviceID id = devices[0].id;

  auto before = driver->GetDeviceState(id);
  REQUIRE(before.has_value());
  CHECK((before->buttons & rex::hid::kPadA) == 0);

  REQUIRE(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true));
  auto after = driver->GetDeviceState(id);
  REQUIRE(after.has_value());
  CHECK((after->buttons & rex::hid::kPadA) != 0);

  SDL_CloseJoystick(joystick);
  SDL_DetachVirtualJoystick(instance);
  driver->Shutdown();
  context.RunPosted();
}
