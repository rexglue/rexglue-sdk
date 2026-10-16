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

REX_TEST_CASE_METHOD(Fixture, "rexgpu-xenos alone registers the neutral tables", "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.gpu.backend = "null";
  config.modules = {"rexfilesystem", "rexruntime", "rexgpu-xenos", "rexui-sdl3"};
  auto api = rex::Init(config);
  REQUIRE(api);
  auto& cvars = *api.value()->Get<rex::cvar::ICvarRegistry>();
  CHECK(HasScope(cvars, "gpu"));
  CHECK(HasScope(cvars, "xenos"));
  CHECK_FALSE(HasScope(cvars, "d3d12"));
  CHECK_FALSE(HasScope(cvars, "vulkan"));
  CHECK(cvars.Enumerate("gpu").size() == 8);
  CHECK(cvars.Enumerate("xenos").size() == 23);
  CHECK(cvars.Get("gpu.present_letterbox"));
  CHECK(cvars.Get("xenos.present_from_non_ui_thread"));
  CHECK(cvars.Get("ui.window_width"));
}

REX_TEST_CASE_METHOD(Fixture, "rexgpu-xenos with the Vulkan library owns the split tables",
                     "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.gpu.backend = "vulkan";
  config.modules = {"rexfilesystem", "rexruntime", "rexgpu-xenos", "rexgpu-xenos-vulkan",
                    "rexui-sdl3"};
  auto api = rex::Init(config);
  REQUIRE(api);
  auto& cvars = *api.value()->Get<rex::cvar::ICvarRegistry>();
  CHECK(cvars.Enumerate("gpu").size() == 9);
  CHECK(cvars.Enumerate("xenos").size() == 49);
  CHECK(cvars.Enumerate("vulkan").size() == 12);
  CHECK_FALSE(HasScope(cvars, "d3d12"));
  CHECK(cvars.Get("xenos.vulkan_pipeline_creation_threads"));
  CHECK_FALSE(cvars.Get("xenos.d3d12_bindless"));
  CHECK(cvars.Get("gpu.anisotropic_override"));
  CHECK_FALSE(cvars.Get("gpu.debug_markers"));
}

#if REX_PLATFORM_WIN32
REX_TEST_CASE_METHOD(Fixture, "rexgpu-xenos with the D3D12 library owns the split tables",
                     "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.gpu.backend = "d3d12";
  config.modules = {"rexfilesystem", "rexruntime", "rexgpu-xenos", "rexgpu-xenos-d3d12",
                    "rexui-sdl3"};
  auto api = rex::Init(config);
  REQUIRE(api);
  auto& cvars = *api.value()->Get<rex::cvar::ICvarRegistry>();
  CHECK(cvars.Enumerate("gpu").size() == 10);
  CHECK(cvars.Enumerate("xenos").size() == 52);
  CHECK(cvars.Enumerate("d3d12").size() == 5);
  CHECK_FALSE(HasScope(cvars, "vulkan"));
  CHECK(cvars.Get("xenos.d3d12_bindless"));
  CHECK(cvars.Get("gpu.debug_markers"));
}

REX_TEST_CASE_METHOD(Fixture, "Both API libraries together fail naming both", "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.modules = {"rexfilesystem", "rexruntime", "rexgpu-xenos", "rexgpu-xenos-d3d12",
                    "rexgpu-xenos-vulkan"};
  auto api = rex::Init(config);
  REQUIRE_FALSE(api);
  CHECK(api.error().message.find("rexgpu-xenos-d3d12") != std::string::npos);
  CHECK(api.error().message.find("rexgpu-xenos-vulkan") != std::string::npos);
}
#endif

REX_TEST_CASE_METHOD(Fixture, "A value set on each table group reaches its owner", "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.gpu.backend = "vulkan";
  config.modules = {"rexfilesystem", "rexruntime", "rexgpu-xenos", "rexgpu-xenos-vulkan"};
  auto api = rex::Init(config);
  REQUIRE(api);
  auto& cvars = *api.value()->Get<rex::cvar::ICvarRegistry>();
  CHECK(cvars.Set("gpu.present_dither", "true"));
  CHECK(cvars.Set("xenos.dump_shaders", "dump_dir"));
  CHECK(cvars.Set("xenos.native_2x_msaa", "true"));
  CHECK(cvars.Set("xenos.vulkan_pipeline_creation_threads", "3"));
  CHECK(cvars.Set("vulkan.log_debug_messages", "false"));
  CHECK(cvars.Get("gpu.present_dither").value_or("") == "true");
  CHECK(cvars.Get("xenos.dump_shaders").value_or("") == "dump_dir");
  CHECK(cvars.Get("xenos.native_2x_msaa").value_or("") == "true");
  CHECK(cvars.Get("xenos.vulkan_pipeline_creation_threads").value_or("") == "3");
  CHECK(cvars.Get("vulkan.log_debug_messages").value_or("") == "false");
}

REX_TEST_CASE_METHOD(Fixture, "A host API library without the neutral module fails Init",
                     "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.gpu.backend = "vulkan";
  config.modules = {"rexfilesystem", "rexruntime", "rexgpu-xenos-vulkan"};
  auto api = rex::Init(config);
  REQUIRE_FALSE(api);
  CHECK(api.error().message.find("rex.gpu.xenos") != std::string::npos);
}

REX_TEST_CASE_METHOD(Fixture, "A host API library fails Connect when gpu.backend names another",
                     "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.gpu.backend = "null";
  config.modules = {"rexfilesystem", "rexruntime", "rexgpu-xenos", "rexgpu-xenos-vulkan"};
  auto api = rex::Init(config);
  REQUIRE_FALSE(api);
  CHECK(api.error().message.find("gpu.backend") != std::string::npos);
}

REX_TEST_CASE_METHOD(Fixture, "Without a GPU module the GPU scopes are absent and ui stays",
                     "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.modules = {"rexfilesystem", "rexruntime", "rexui-sdl3"};
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
  config.gpu.backend = "null";
  config.modules = {"rexfilesystem", "rexruntime", "rexgpu-xenos"};
  auto api = rex::Init(config);
  REQUIRE(api);
  auto& cvars = *api.value()->Get<rex::cvar::ICvarRegistry>();
  CHECK_FALSE(cvars.Set("gpu.d3d12_bindless", "false"));
  CHECK_FALSE(cvars.Set("ui.present_letterbox", "false"));
}

REX_TEST_CASE_METHOD(Fixture, "The runtime reserves apu and rexapu-sdl3 adds apu.mute",
                     "[gpu][cvar]") {
  rex::platform::SetEnvironmentValue("SDL_AUDIO_DRIVER", "dummy");
  rex::Config config = BaseConfig();
  config.modules = {"rexfilesystem", "rexruntime", "rexapu-sdl3"};
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
  config.modules = {"rexfilesystem", "rexruntime"};
  config.apu.max_queued_frames = 16;
  auto api = rex::Init(config);
  REQUIRE(api);
  auto& cvars = *api.value()->Get<rex::cvar::ICvarRegistry>();
  CHECK(cvars.Get("apu.max_queued_frames").value_or("") == "16");
}

REX_TEST_CASE_METHOD(Fixture, "rexhid-sdl3 adds the 33 hid cvars to the reserved scope",
                     "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.modules = {"rexfilesystem", "rexruntime", "rexhid-sdl3"};
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
  config.modules = {"rexfilesystem", "rexruntime", "rexhid-sdl3", "rexhid-xinput"};
  auto api = rex::Init(config);
  REQUIRE_FALSE(api);
  CHECK(api.error().message.find("rex.hid.backend is served by both") != std::string::npos);
}
#endif

REX_TEST_CASE_METHOD(Fixture, "Without a hid module the hid scope is reserved and empty",
                     "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.modules = {"rexfilesystem", "rexruntime"};
  auto api = rex::Init(config);
  REQUIRE(api);
  auto& cvars = *api.value()->Get<rex::cvar::ICvarRegistry>();
  CHECK(HasScope(cvars, "hid"));
  CHECK(cvars.Enumerate("hid").empty());
}

REX_TEST_CASE_METHOD(Fixture, "The default host modules bring the SDK to 70 cvars", "[gpu][cvar]") {
  rex::platform::SetEnvironmentValue("SDL_AUDIO_DRIVER", "dummy");
  rex::Config config = BaseConfig();
  config.modules = {"rexfilesystem", "rexruntime", "rexapu-sdl3", "rexhid-sdl3", "rexui-sdl3"};
  auto api = rex::Init(config);
  REQUIRE(api);
  auto& cvars = *api.value()->Get<rex::cvar::ICvarRegistry>();
  CHECK(cvars.Enumerate().size() == 70);
}
