/**
 * @file        tests/unit/base/core_test.cpp
 * @brief       rexbase stands alone with an app and no provider
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rex/logging/logger.h>
#include <rex/memory/host.h>
#include <rex/platform.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/logging/log_category.h>

#include "base/app_harness.h"

REX_TEST_CASE("A command line value sets perf.profiler above the title value", "[base]") {
  rex::test::HarnessApp app;
  app.configure = [](rex::SettingsRegistry&) -> rex::Result<void> {
    auto found = REX_SETTINGS().Find("perf.profiler");
    REQUIRE(found);
    REQUIRE(found.value()->SetText(rex::SettingLayer::kTitle, "true"));
    return rex::Ok();
  };
  app.init = []() -> rex::Result<void> {
    auto found = REX_SETTINGS().Find("perf.profiler");
    REQUIRE(found);
    CHECK(found.value()->GetText() == "false");
    CHECK(found.value()->GetLayer() == rex::SettingLayer::kCommandLine);
    return rex::Ok();
  };
  REQUIRE(app.Start({"--perf.profiler=false"}));
  app.Finish();
}

REX_TEST_CASE("The perf.alloc_stack_depth setting goes through the allocator setter",
              "[base][memory]") {
  rex::test::RunInInit([] {
    rex::memory::SetAllocStackDepth(0);
    REQUIRE(REX_SETTINGS().SetText("perf.alloc_stack_depth", "7"));
    CHECK(rex::memory::GetAllocStackDepth() == 7);
    REQUIRE(REX_SETTINGS().SetText("perf.alloc_stack_depth", "0"));
    CHECK(rex::memory::GetAllocStackDepth() == 0);
    rex::memory::SetAllocStackDepth(5);
    CHECK(rex::memory::GetAllocStackDepth() == 5);
    rex::memory::SetAllocStackDepth(0);
  });
}

REX_TEST_CASE("Init applies the perf.alloc_stack_depth value it was given", "[base][memory]") {
  rex::memory::SetAllocStackDepth(0);
  rex::test::RunInInit([] { CHECK(rex::memory::GetAllocStackDepth() == 3); },
                       {"--perf.alloc_stack_depth=3"});
  rex::memory::SetAllocStackDepth(0);
}

REX_TEST_CASE("The alloc stack depth watch ends with the app", "[base][memory]") {
  rex::test::RunInInit([] {});
  rex::memory::SetAllocStackDepth(2);
  REQUIRE(REX_SETTINGS().SetText("perf.alloc_stack_depth", "9"));
  CHECK(rex::memory::GetAllocStackDepth() == 2);
  REQUIRE(REX_SETTINGS().SetText("perf.alloc_stack_depth", "0"));
  rex::memory::SetAllocStackDepth(0);
}

#if !REX_PLATFORM_WIN32
REX_TEST_CASE("Init logs the open file limit", "[base]") {
  std::vector<rex::log::CapturedMessage> entries;
  rex::log::LogCaptureSink sink;
  auto sink_guard = REX_LOGGER().AddSink(sink);
  rex::test::RunInInit([] {});
  sink_guard.Reset();
  sink.CopySince(0, entries);
  CHECK(std::ranges::any_of(entries, [](const rex::log::CapturedMessage& entry) {
    return entry.text.find("open file limit") != std::string::npos;
  }));
}
#endif

REX_TEST_CASE("Logging writes through a capture sink with no module loaded", "[base]") {
  std::vector<rex::log::CapturedMessage> entries;
  rex::test::RunInInit([&] {
    rex::Logger& logger = REX_LOGGER();
    rex::log::LogCaptureSink sink;
    auto sink_guard = logger.AddSink(sink);
    REXLOG_INFO("core test entry {}", 42);
    sink.CopySince(0, entries);
    sink_guard.Reset();
  });
  REQUIRE(std::ranges::any_of(entries, [](const rex::log::CapturedMessage& entry) {
    return entry.text.find("core test entry 42") != std::string::npos;
  }));
}
