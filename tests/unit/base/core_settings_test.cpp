/**
 * @file        tests/unit/base/core_settings_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <string_view>

#include <rex/chrono.h>
#include <rex/memory/host.h>
#include <rex/path_settings.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/console_command.h>

#include "base/app_harness.h"
#include "gpu/xenos/host/d3d12/d3d12_settings.h"
#include "gpu/xenos/host/vulkan/vulkan_settings.h"
#include "gpu/xenos/xenos_settings.h"

REX_TEST_CASE("no_scaling follows the setting", "[base][settings]") {
  rex::test::RunInInit([] {
    CHECK_FALSE(rex::Clock::IsNoScaling());
    REQUIRE(REX_SETTINGS().SetText("clock.no_scaling", "true"));
    CHECK(rex::Clock::IsNoScaling());
    REQUIRE(REX_SETTINGS().SetText("clock.no_scaling", "false"));
    CHECK_FALSE(rex::Clock::IsNoScaling());
  });
}

REX_TEST_CASE("alloc_stack_depth has a range", "[base][settings]") {
  rex::test::RunInInit([] {
    CHECK_FALSE(REX_SETTINGS().SetText("perf.alloc_stack_depth", "33"));
    CHECK(REX_SETTINGS().SetText("perf.alloc_stack_depth", "32"));
    CHECK(REX_SETTINGS().Find("perf.alloc_stack_depth").value()->GetText() == "32");
  });
}

REX_TEST_CASE("path settings keep their app.* keys and defaults", "[base][settings]") {
  rex::test::RunInInit([] {
    rex::PathSettings& paths = REX_PATHS();
    CHECK(paths.config_file.GetName() == "app.config_file");
    CHECK(paths.user_root.GetName() == "app.user_root");
    CHECK(paths.cache_root.GetName() == "app.cache_root");
    CHECK(paths.config_file.GetDefaultText() == "{app.root}/{app.name}.toml");
    CHECK(paths.user_root.GetDefaultText() == "{app.root}/user");
    CHECK(paths.cache_root.GetDefaultText().empty());
    CHECK(REX_SETTINGS().Find("app.user_root").value() == &paths.user_root);
  });
}

REX_TEST_CASE("profiler cannot change after startup", "[base][settings]") {
  rex::test::RunInInit([] {
    CHECK_FALSE(REX_SETTINGS().SetText("perf.profiler", "true"));
    CHECK(REX_SETTINGS().Find("perf.profiler").value()->GetText() == "false");
  });
}

namespace {

struct XenosOwner : rex::gpu::XenosSettings {};

bool IsLowercaseDotted(std::string_view name) {
  if (name.empty() || name.front() == '.' || name.back() == '.' || name.find("..") != name.npos) {
    return false;
  }
  return name.find('.') != name.npos && std::ranges::all_of(name, [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
         });
}

}  // namespace

REX_TEST_CASE("every SDK setting name is lowercase dotted", "[base][settings]") {
  rex::test::RunInInit([] {
    auto& settings = REX_SETTINGS();
    XenosOwner xenos;
    rex::gpu::vulkan::VulkanSettings vulkan;
    rex::gpu::d3d12::D3D12Settings d3d12;
    REQUIRE(settings.Register(xenos));
    REQUIRE(settings.Register(vulkan));
    REQUIRE(settings.Register(d3d12));
    const auto found = settings.Enumerate();
    CHECK(found.size() > 3);
    for (const rex::SettingBase* setting : found) {
      INFO(setting->GetName());
      CHECK(IsLowercaseDotted(setting->GetName()));
    }
    for (const rex::ConsoleCommand* command : settings.EnumerateCommands()) {
      INFO(command->GetName());
      CHECK(IsLowercaseDotted(command->GetName()));
    }
    settings.Unregister(d3d12);
    settings.Unregister(vulkan);
    settings.Unregister(xenos);
  });
}
