/**
 * @file        tests/unit/apu/sdl3_audio_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <rex/apu/audio_driver.h>
#include <rex/logging/logger.h>
#include <rex/platform/process.h>
#include <rex/settings_registry.h>

#include "base/driver_context.h"
#include "base/module_group.h"
#include "base/module_registration.h"

namespace {

struct Fixture {
  Fixture() {
    rex::platform::SetEnvironmentValue("SDL_AUDIO_DRIVER", "dummy");
    REQUIRE(group.Load("rexapu-sdl3").has_value());
    REQUIRE(group.Connect().has_value());
  }

  std::unique_ptr<rex::apu::IAudioDriver> Create() {
    for (const rex::DriverRegistration* registration :
         rex::test::GetRegistrations(group, rex::DriverType::kAudio)) {
      if (registration->name == "sdl3") {
        std::unique_ptr<rex::IDriver> driver = registration->create();
        return std::unique_ptr<rex::apu::IAudioDriver>(
            static_cast<rex::apu::IAudioDriver*>(driver.release()));
      }
    }
    return nullptr;
  }

  rex::ModuleGroup group;
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "sdl3 provider registrations sdl3", "[apu][sdl3]") {
  auto driver = Create();
  REQUIRE(driver);
  CHECK(driver->GetName() == "sdl3");
}

REX_TEST_CASE_METHOD(Fixture, "sdl3 opens 2 or 6 channels", "[apu][sdl3]") {
  auto driver = Create();
  REQUIRE(driver);
  rex::AppDriverContext context(REX_SETTINGS(), REX_LOGGER());
  REQUIRE(driver->Initialize(context).has_value());
  std::atomic<int> pulls{0};
  std::atomic<bool> aligned{true};
  auto channels = driver->OpenDevice(6, [&](std::span<rex::f32> samples, rex::u32 count) {
    if (samples.empty() || samples.size() % count != 0) {
      aligned = false;
    }
    std::ranges::fill(samples, 0.0f);
    ++pulls;
  });
  REQUIRE(channels.has_value());
  CHECK((*channels == 2 || *channels == 6));
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (pulls < 5 && std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(pulls >= 5);
  CHECK(aligned);
  driver->CloseDevice();
  driver->Shutdown();
}
