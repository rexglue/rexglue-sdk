/**
 * @file        runtime/thread_wait_test.cpp
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
#include <memory>
#include <vector>

#include <rex/platform/thread.h>

#include "runtime/thread_wait.h"

namespace {

using namespace std::chrono_literals;

struct HostThread {
  std::unique_ptr<rex::platform::Thread> host;
  std::atomic<int> force_stops{0};
  rex::platform::Thread* thread() const { return host.get(); }
  void ForceStop() { ++force_stops; }
};

std::shared_ptr<HostThread> Sleeper(std::chrono::milliseconds duration) {
  auto thread = std::make_shared<HostThread>();
  thread->host = rex::platform::Thread::Create({}, [duration] { rex::platform::Sleep(duration); });
  return thread;
}

}  // namespace

TEST_CASE("A running thread is reported each interval until it ends", "[runtime][thread_wait]") {
  std::vector<std::shared_ptr<HostThread>> threads{Sleeper(0ms), Sleeper(120ms)};
  std::atomic<int> reports{0};
  rex::runtime::WaitForThreadsToStop(threads, 50ms, [&](const auto&) { ++reports; });
  CHECK(reports.load() >= 1);
  CHECK(reports.load() <= 3);
}

TEST_CASE("A thread past its timeout is forced to stop every few ms", "[runtime][thread_wait]") {
  std::vector<std::shared_ptr<HostThread>> threads{Sleeper(150ms)};
  rex::runtime::WaitForThreadsToStop(threads, 50ms, [](const auto&) {});
  CHECK(threads[0]->force_stops.load() >= 5);
}

TEST_CASE("A thread that ends before its timeout is never forced", "[runtime][thread_wait]") {
  std::vector<std::shared_ptr<HostThread>> threads{Sleeper(20ms)};
  rex::runtime::WaitForThreadsToStop(threads, 1s, [](const auto&) {});
  CHECK(threads[0]->force_stops.load() == 0);
}

TEST_CASE("Ended threads end the wait at once", "[runtime][thread_wait]") {
  std::vector<std::shared_ptr<HostThread>> threads{Sleeper(0ms), Sleeper(0ms)};
  rex::platform::Wait(threads[0]->thread(), false);
  rex::platform::Wait(threads[1]->thread(), false);
  std::atomic<int> reports{0};
  rex::runtime::WaitForThreadsToStop(threads, 5s, [&](const auto&) { ++reports; });
  CHECK(reports.load() == 0);
}
