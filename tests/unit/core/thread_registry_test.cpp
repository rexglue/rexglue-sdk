/**
 * @file        tests/unit/core/thread_registry_test.cpp
 * @brief       Unit tests for the SDK host thread registry
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <latch>

#include <rex/platform/thread.h>

using namespace std::chrono_literals;

TEST_CASE("Spawn registers a thread until it ends", "[thread_registry]") {
  const rex::u64 before = rex::platform::detail::LiveCount();
  std::latch started(1);
  std::latch release(1);
  {
    auto worker = rex::platform::Spawn("registry test", [&](const std::stop_token&) {
      started.count_down();
      release.wait();
    });
    started.wait();
    REQUIRE(rex::platform::detail::LiveCount() == before + 1);
    release.count_down();
  }
  REQUIRE(rex::platform::detail::LiveCount() == before);
}

TEST_CASE("A thread leaving through Thread::Exit leaves the registry", "[thread_registry]") {
  const rex::u64 before = rex::platform::detail::LiveCount();
  auto thread = rex::platform::Thread::Create({}, [] { rex::platform::Thread::Exit(0); });
  REQUIRE(thread != nullptr);
  REQUIRE(rex::platform::Wait(thread.get(), false) == rex::platform::WaitResult::kSuccess);
  REQUIRE(rex::platform::detail::LiveCount() == before);
}

TEST_CASE("Terminate removes the terminated thread", "[thread_registry]") {
  const rex::u64 before = rex::platform::detail::LiveCount();
  std::latch started(1);
  std::atomic<bool> stop{false};
  auto thread = rex::platform::Thread::Create({}, [&] {
    started.count_down();
    while (!stop.load()) {
      rex::platform::Sleep(1ms);
    }
  });
  REQUIRE(thread != nullptr);
  started.wait();
  REQUIRE(rex::platform::detail::LiveCount() == before + 1);
  thread->Terminate(0);
  rex::platform::Wait(thread.get(), false);
  REQUIRE(rex::platform::detail::LiveCount() == before);
}

TEST_CASE("ShutdownTimerQueue without a queue does nothing", "[thread_registry]") {
  const rex::u64 before = rex::platform::detail::LiveCount();
  rex::platform::detail::ShutdownTimerQueue();
  REQUIRE(rex::platform::detail::LiveCount() == before);
}
