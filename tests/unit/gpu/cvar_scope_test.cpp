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

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>

#include <rex/api.h>
#include <rex/cvar/registry.h>

namespace {

bool HasScope(const rex::cvar::ICvarRegistry& cvars, std::string_view scope) {
  return std::ranges::contains(cvars.Scopes(), scope);
}

rex::Config BaseConfig() {
  rex::Config config;
  config.paths.config_file.clear();
  return config;
}

struct Fixture {
  Fixture() { rex::Shutdown(); }
  ~Fixture() { rex::Shutdown(); }
};

}  // namespace

TEST_CASE_METHOD(Fixture, "rex-gpu-xenos registers the 4 GPU scopes", "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime", "rex-gpu-xenos"};
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

TEST_CASE_METHOD(Fixture, "Without a GPU module the GPU scopes are absent and ui stays",
                 "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime"};
  auto api = rex::Init(config);
  REQUIRE(api);
  auto& cvars = *api.value()->Get<rex::cvar::ICvarRegistry>();
  for (std::string_view scope : {"gpu", "xenos", "d3d12", "vulkan"}) {
    CHECK_FALSE(HasScope(cvars, scope));
  }
  CHECK(cvars.Enumerate().size() == 70);
  CHECK(cvars.Enumerate("ui").size() == 10);
  CHECK(cvars.Get("ui.window_width"));
}

TEST_CASE_METHOD(Fixture, "An old GPU key is unknown after the move", "[gpu][cvar]") {
  rex::Config config = BaseConfig();
  config.modules = {"rex-filesystem", "rex-runtime", "rex-gpu-xenos"};
  auto api = rex::Init(config);
  REQUIRE(api);
  auto& cvars = *api.value()->Get<rex::cvar::ICvarRegistry>();
  CHECK_FALSE(cvars.Set("gpu.d3d12_bindless", "false"));
  CHECK_FALSE(cvars.Set("ui.present_letterbox", "false"));
}
