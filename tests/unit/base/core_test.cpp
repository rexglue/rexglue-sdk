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
#include <atomic>
#include <rex/platform/thread.h>
#include <rex/platform/sync.h>
#include <stdexcept>
#include <chrono>
#include <latch>
#include <rex/platform/timer.h>
#include <rex/chrono.h>
#include <rex/path_settings.h>
#include <rex/console_command.h>
#include "gpu/xenos/host/d3d12/d3d12_settings.h"
#include "gpu/xenos/host/vulkan/vulkan_settings.h"
#include "gpu/xenos/xenos_settings.h"

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

REX_TEST_CASE("A callback queued to an exited thread is dropped quietly", "[base][thread]") {
  rex::test::RunInInit(
      [] {
        rex::Logger& logger = REX_LOGGER();
        auto thread = rex::platform::Thread::Create({}, [] {});
        REQUIRE(thread);
        REQUIRE(rex::platform::Wait(thread.get(), false) == rex::platform::WaitResult::kSuccess);

        auto ran = std::make_shared<std::atomic<bool>>(false);
        auto token = std::make_shared<int>(0);
        std::weak_ptr<int> alive = token;
        rex::log::LogCaptureSink sink;
        auto sink_guard = logger.AddSink(sink);
        thread->QueueUserCallback([ran, token = std::move(token)] { ran->store(true); });
        std::vector<rex::log::CapturedMessage> messages;
        sink.CopySince(0, messages);
        sink_guard.Reset();

        CHECK(alive.expired());
        CHECK_FALSE(ran->load());
        CHECK(std::ranges::none_of(messages, [](const rex::log::CapturedMessage& message) {
          return message.level >= rex::log::Level::kWarn;
        }));
        thread.reset();
      },
      {"--apu.driver=none", "--hid.drivers=none", "--ui.driver=none"});
}

REX_TEST_CASE("NoThrow logs an exception from its step and returns", "[base][no_throw]") {
  bool ran = false;
  std::vector<rex::log::CapturedMessage> messages;
  rex::test::RunInInit([&] {
    rex::Logger& logger = REX_LOGGER();
    rex::log::LogCaptureSink sink;
    auto sink_guard = logger.AddSink(sink);

    rex::log::NoThrow(rex::log::core, [&] { ran = true; });
    rex::log::NoThrow(rex::log::core, [] { throw std::runtime_error("socket already closed"); });

    sink.CopySince(0, messages);
    sink_guard.Reset();
  });

  CHECK(ran);
  CHECK(std::ranges::any_of(messages, [](const rex::log::CapturedMessage& message) {
    return message.text == "teardown step failed, socket already closed";
  }));
}

REX_TEST_CASE("NoThrow is noexcept", "[base][no_throw]") {
  STATIC_REQUIRE(noexcept(rex::log::NoThrow(rex::log::core, [] {})));
}

using namespace std::chrono_literals;

REX_TEST_CASE("Spawn registers a thread until it ends", "[base][thread_registry]") {
  const rex::u64 before = rex::platform::detail::GetThreadCount();
  std::latch started(1);
  std::latch release(1);
  {
    auto worker = rex::platform::Spawn("registry test", [&](const std::stop_token&) {
      started.count_down();
      release.wait();
    });
    started.wait();
    REQUIRE(rex::platform::detail::GetThreadCount() == before + 1);
    release.count_down();
  }
  REQUIRE(rex::platform::detail::GetThreadCount() == before);
}

REX_TEST_CASE("A thread leaving through Thread::Exit leaves the registry",
              "[base][thread_registry]") {
  const rex::u64 before = rex::platform::detail::GetThreadCount();
  auto thread = rex::platform::Thread::Create({}, [] { rex::platform::Thread::Exit(0); });
  REQUIRE(thread != nullptr);
  REQUIRE(rex::platform::Wait(thread.get(), false) == rex::platform::WaitResult::kSuccess);
  REQUIRE(rex::platform::detail::GetThreadCount() == before);
}

REX_TEST_CASE("A thread asked to stop leaves the registry", "[base][thread_registry]") {
  const rex::u64 before = rex::platform::detail::GetThreadCount();
  std::latch started(1);
  std::atomic<bool> stop{false};
  auto thread = rex::platform::Thread::Create({}, [&] {
    started.count_down();
    while (!stop.load()) {
      rex::platform::SleepFor(1ms);
    }
  });
  REQUIRE(thread != nullptr);
  started.wait();
  REQUIRE(rex::platform::detail::GetThreadCount() == before + 1);
  stop.store(true);
  rex::platform::Wait(thread.get(), false);
  REQUIRE(rex::platform::detail::GetThreadCount() == before);
}

REX_TEST_CASE("App Shutdown joins the timer queue thread", "[base][thread_registry]") {
  const rex::u64 before = rex::platform::detail::GetThreadCount();
  rex::test::RunInInit([] {
    const auto item = rex::platform::QueueTimerOnce(
        [](void*) {}, nullptr, rex::platform::TimerQueueWaitItem::clock::now() + 1h);
    REQUIRE_FALSE(item.expired());
  });
  CHECK(rex::platform::detail::GetThreadCount() == before);
}

REX_TEST_CASE("ShutdownTimerQueue without a queue does nothing", "[base][thread_registry]") {
  const rex::u64 before = rex::platform::detail::GetThreadCount();
  rex::platform::detail::ShutdownTimerQueue();
  REQUIRE(rex::platform::detail::GetThreadCount() == before);
}

namespace core_settings_tests {

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

}  // namespace core_settings_tests
