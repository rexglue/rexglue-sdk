/**
 * @file        tests/unit/base/thread_registry_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <chrono>
#include <latch>

#include <rex/platform/thread.h>
#include <rex/platform/sync.h>
#include <rex/platform/timer.h>

#include "base/app_harness.h"

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
