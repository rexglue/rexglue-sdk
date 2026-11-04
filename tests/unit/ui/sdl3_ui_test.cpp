/**
 * @file        tests/unit/ui/sdl3_ui_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <memory>
#include <string>

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_video.h>

#include <rex/driver.h>
#include <rex/driver_registration.h>
#include <rex/logging/logger.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/ui/ui_driver.h>
#include <rex/ui/window.h>

#include "base/driver_context.h"
#include "base/module_group.h"
#include "base/module_registration.h"
#include "ui/sdl3/video_scope.h"

namespace {

struct Fixture {
  Fixture() {
    REQUIRE(group.Load("rexui-sdl3").has_value());
    REQUIRE(group.Connect().has_value());
  }

  std::unique_ptr<rex::ui::IUIDriver> Create() {
    for (const rex::DriverRegistration* registration :
         rex::test::GetRegistrations(group, rex::DriverType::kUI)) {
      if (registration->name == "sdl3") {
        std::unique_ptr<rex::IDriver> driver = registration->create();
        return std::unique_ptr<rex::ui::IUIDriver>(
            static_cast<rex::ui::IUIDriver*>(driver.release()));
      }
    }
    return nullptr;
  }

  rex::ModuleGroup group;
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "sdl3 registrations a UI driver named sdl3", "[ui][sdl3]") {
  auto driver = Create();
  REQUIRE(driver);
  CHECK(driver->GetName() == "sdl3");
}

REX_TEST_CASE_METHOD(Fixture, "the video driver setting is ui.sdl.video_driver with an alias",
                     "[ui][sdl3]") {
  auto driver = Create();
  REQUIRE(driver);
  rex::AppDriverContext context(REX_SETTINGS(), REX_LOGGER());
  REQUIRE(driver->Initialize(context).has_value());
  auto found = REX_SETTINGS().Find("ui.sdl.video_driver");
  REQUIRE(found.has_value());
  REQUIRE(
      REX_SETTINGS().SetText(rex::SettingLayer::kTitle, "ui.video_driver", "dummy").has_value());
  CHECK((*found)->GetText() == "dummy");
  driver->Shutdown();
}

REX_TEST_CASE("a video scope pins the running video driver", "[ui][sdl3]") {
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy,offscreen");
  {
    rex::ui::sdl3::VideoScope video;
    REQUIRE(video.IsOk());
    const char* pinned = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    REQUIRE(pinned != nullptr);
    CHECK(std::string(pinned) == std::string(SDL_GetCurrentVideoDriver()));
  }
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
}

REX_TEST_CASE_METHOD(Fixture, "a window is resizable and scaled", "[ui][sdl3]") {
  auto driver = Create();
  REQUIRE(driver);
  REQUIRE(REX_SETTINGS()
              .SetText(rex::SettingLayer::kTitle, "ui.sdl.video_driver", "offscreen")
              .has_value());
  rex::AppDriverContext context(REX_SETTINGS(), REX_LOGGER());
  REQUIRE(driver->Initialize(context).has_value());
  if (!driver->HasDisplay()) {
    driver->Shutdown();
    SKIP("no display is available");
  }
  REQUIRE(driver->OpenWindow(rex::ui::WindowConfig{}).has_value());
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  REQUIRE(windows != nullptr);
  REQUIRE(count > 0);
  const SDL_WindowFlags flags = SDL_GetWindowFlags(windows[0]);
  SDL_free(static_cast<void*>(windows));
  CHECK((flags & SDL_WINDOW_RESIZABLE) != 0);
  CHECK((flags & SDL_WINDOW_HIGH_PIXEL_DENSITY) != 0);
  CHECK((flags & SDL_WINDOW_VULKAN) == 0);
  driver->Shutdown();
}
