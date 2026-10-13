/**
 * @file        tests/unit/gpu/cvar_scope_test.cpp
 * @brief       The GPU backend module registers the gpu, xenos, d3d12 and vulkan scopes
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "support/base_config.h"

#include <algorithm>
#include <string>
#include <string_view>

#include <rex/api.h>
#include <rex/cvar/registry.h>
#include <rex/platform.h>
#include <rex/platform/env.h>

namespace {

using rex::test::BaseConfig;

bool HasScope(const rex::cvar::ICvarRegistry& cvars, std::string_view scope) {
  return std::ranges::contains(cvars.GetScopes(), scope);
}

struct Fixture {
  Fixture() { rex::Shutdown(); }
  ~Fixture() { rex::Shutdown(); }
};

}  // namespace

REX_TEST_CASE_METHOD(Fixture, "rex-gpu-xenos registers the 4 GPU scopes", "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime", "rex-gpu-xenos", "rex-ui-sdl3"};
  auto api = rex::Init(config);
  REQUIRE(api);
  auto& cvars = *api.value()->Get<rex::cvar::ICvarRegistry>();
  for (std::string_view scope : {"gpu", "xenos", "d3d12", "vulkan"}) {
    CHECK(HasScope(cvars, scope));
  }
  CHECK(cvars.Enumerate("gpu").size() == 10);
  CHECK(cvars.Enumerate("xenos").size() == 65);
  CHECK(cvars.Enumerate("d3d12").size() == 5);
  CHECK(cvars.Enumerate("vulkan").size() == 12);
  CHECK(cvars.Get("xenos.d3d12_bindless"));
  CHECK_FALSE(cvars.Get("gpu.d3d12_bindless"));
  CHECK(cvars.Get("gpu.present_letterbox"));
  CHECK(cvars.Get("xenos.present_from_non_ui_thread"));
  CHECK(cvars.Get("ui.window_width"));
}

REX_TEST_CASE_METHOD(Fixture, "Without a GPU module the GPU scopes are absent and ui stays",
                     "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime", "rex-ui-sdl3"};
  auto api = rex::Init(config);
  REQUIRE(api);
  auto& cvars = *api.value()->Get<rex::cvar::ICvarRegistry>();
  for (std::string_view scope : {"gpu", "xenos", "d3d12", "vulkan"}) {
    CHECK_FALSE(HasScope(cvars, scope));
  }
  CHECK(cvars.Enumerate().size() == 36);
  CHECK(cvars.Enumerate("ui").size() == 10);
  CHECK(cvars.Get("ui.window_width"));
}

REX_TEST_CASE_METHOD(Fixture, "An old GPU key is unknown after the move", "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime", "rex-gpu-xenos"};
  auto api = rex::Init(config);
  REQUIRE(api);
  auto& cvars = *api.value()->Get<rex::cvar::ICvarRegistry>();
  CHECK_FALSE(cvars.Set("gpu.d3d12_bindless", "false"));
  CHECK_FALSE(cvars.Set("ui.present_letterbox", "false"));
}

REX_TEST_CASE_METHOD(Fixture, "The runtime reserves apu and rex-apu-sdl3 adds apu.mute",
                     "[gpu][cvar]") {
  rex::platform::SetEnvironmentValue("SDL_AUDIO_DRIVER", "dummy");
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime", "rex-apu-sdl3"};
  auto api = rex::Init(config);
  REQUIRE(api);
  auto& cvars = *api.value()->Get<rex::cvar::ICvarRegistry>();
  CHECK(cvars.Enumerate("apu").size() == 2);
  CHECK(cvars.Get("apu.mute"));
  CHECK(cvars.Get("apu.max_queued_frames"));
  CHECK(cvars.Get("runtime.ffmpeg_verbose"));
  CHECK_FALSE(cvars.Set("audio.mute", "true"));
}

REX_TEST_CASE_METHOD(Fixture, "apu.max_queued_frames takes its default from Config::apu",
                     "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime"};
  config.apu.max_queued_frames = 16;
  auto api = rex::Init(config);
  REQUIRE(api);
  auto& cvars = *api.value()->Get<rex::cvar::ICvarRegistry>();
  CHECK(cvars.Get("apu.max_queued_frames").value_or("") == "16");
}

REX_TEST_CASE_METHOD(Fixture, "rex-hid-sdl3 adds the 33 hid cvars to the reserved scope",
                     "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime", "rex-hid-sdl3"};
  auto api = rex::Init(config);
  REQUIRE(api);
  auto& cvars = *api.value()->Get<rex::cvar::ICvarRegistry>();
  CHECK(HasScope(cvars, "hid"));
  CHECK(cvars.Enumerate("hid").size() == 33);
  CHECK(cvars.Get("hid.keybind_a").value_or("") == "Semicolon,Space");
  CHECK_FALSE(cvars.Set("input.keybind_a", "A"));
}

#if REX_PLATFORM_WIN32
REX_TEST_CASE_METHOD(Fixture, "A second hid module fails Init naming the shared interface",
                     "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime", "rex-hid-sdl3", "rex-hid-xinput"};
  auto api = rex::Init(config);
  REQUIRE_FALSE(api);
  CHECK(api.error().message.find("rex.hid.backend is served by both") != std::string::npos);
}
#endif

REX_TEST_CASE_METHOD(Fixture, "Without a hid module the hid scope is reserved and empty",
                     "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime"};
  auto api = rex::Init(config);
  REQUIRE(api);
  auto& cvars = *api.value()->Get<rex::cvar::ICvarRegistry>();
  CHECK(HasScope(cvars, "hid"));
  CHECK(cvars.Enumerate("hid").empty());
}

REX_TEST_CASE_METHOD(Fixture, "The default host modules bring the SDK to 70 cvars", "[gpu][cvar]") {
  rex::platform::SetEnvironmentValue("SDL_AUDIO_DRIVER", "dummy");
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime", "rex-apu-sdl3", "rex-hid-sdl3", "rex-ui-sdl3"};
  auto api = rex::Init(config);
  REQUIRE(api);
  auto& cvars = *api.value()->Get<rex::cvar::ICvarRegistry>();
  CHECK(cvars.Enumerate().size() == 70);
}
