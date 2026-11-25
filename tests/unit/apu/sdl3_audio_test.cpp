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
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_hints.h>

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

REX_TEST_CASE_METHOD(Fixture, "apu.sdl.device_sample_frames sets the device buffer size",
                     "[apu][sdl3]") {
  auto driver = Create();
  REQUIRE(driver);
  REQUIRE(REX_SETTINGS()
              .SetText(rex::SettingLayer::kTitle, "apu.sdl.device_sample_frames", "256")
              .has_value());
  rex::AppDriverContext context(REX_SETTINGS(), REX_LOGGER());
  REQUIRE(driver->Initialize(context).has_value());
  auto found = REX_SETTINGS().Find("apu.sdl.device_sample_frames");
  REQUIRE(found.has_value());
  CHECK((*found)->GetText() == "256");
  const char* hint = SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
  REQUIRE(hint != nullptr);
  CHECK(std::string(hint) == "256");

  auto channels = driver->OpenDevice(
      2, [](std::span<rex::f32> samples, rex::u32) { std::ranges::fill(samples, 0.0f); });
  REQUIRE(channels.has_value());
  SDL_AudioSpec spec{};
  int sample_frames = 0;
  REQUIRE(SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &sample_frames));
  CHECK(sample_frames == 256);

  driver->CloseDevice();
  REQUIRE((*found)->SetText(rex::SettingLayer::kTitle, "0").has_value());
  driver->Shutdown();
  SDL_ResetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
  CHECK_FALSE(REX_SETTINGS().Find("apu.sdl.device_sample_frames").has_value());
}
