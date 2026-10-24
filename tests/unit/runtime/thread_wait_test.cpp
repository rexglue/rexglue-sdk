/**
 * @file        tests/unit/runtime/thread_wait_test.cpp
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
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#include <rex/platform/thread.h>

#include "runtime/thread_wait.h"

namespace {

using namespace std::chrono_literals;

struct HostThread {
  std::unique_ptr<rex::platform::Thread> host;
  std::atomic<int> force_stops{0};
  rex::platform::Thread* GetThread() const { return host.get(); }
  void ForceStop() { ++force_stops; }
};

std::shared_ptr<HostThread> Sleeper(std::chrono::milliseconds duration) {
  auto thread = std::make_shared<HostThread>();
  thread->host = rex::platform::Thread::Create({}, [duration] { rex::platform::Sleep(duration); });
  return thread;
}

struct StubbornThread {
  explicit StubbornThread(int stop_after_force_stops) : stop_after(stop_after_force_stops) {
    host = rex::platform::Thread::Create({}, [this] {
      std::unique_lock lock(mutex);
      released.wait_for(lock, 10s, [this] { return open; });
    });
  }
  rex::platform::Thread* GetThread() const { return host.get(); }
  void ForceStop() {
    if (++force_stops != stop_after) {
      return;
    }
    {
      std::lock_guard lock(mutex);
      open = true;
    }
    released.notify_all();
    rex::platform::Wait(host.get(), false);
  }

  int stop_after;
  std::mutex mutex;
  std::condition_variable released;
  bool open = false;
  std::atomic<int> force_stops{0};
  std::unique_ptr<rex::platform::Thread> host;
};

}  // namespace

REX_TEST_CASE("A running thread is reported each interval until it ends",
              "[runtime][thread_wait]") {
  auto stubborn = std::make_shared<StubbornThread>(11);
  std::vector<std::shared_ptr<StubbornThread>> threads{stubborn};
  std::atomic<int> reports{0};
  rex::runtime::WaitForThreadsToStop(threads, 50ms, [&](const auto&) { ++reports; });
  CHECK(stubborn->force_stops.load() == 11);
  CHECK(reports.load() == 2);
}

REX_TEST_CASE("A thread past its timeout is forced to stop every few ms",
              "[runtime][thread_wait]") {
  auto stubborn = std::make_shared<StubbornThread>(20);
  std::vector<std::shared_ptr<StubbornThread>> threads{stubborn};
  rex::runtime::WaitForThreadsToStop(threads, 50ms, [](const auto&) {});
  CHECK(stubborn->force_stops.load() == 20);
}

REX_TEST_CASE("A thread that ends before its timeout is never forced", "[runtime][thread_wait]") {
  std::vector<std::shared_ptr<HostThread>> threads{Sleeper(20ms)};
  rex::runtime::WaitForThreadsToStop(threads, 1s, [](const auto&) {});
  CHECK(threads[0]->force_stops.load() == 0);
}

REX_TEST_CASE("Ended threads end the wait at once", "[runtime][thread_wait]") {
  std::vector<std::shared_ptr<HostThread>> threads{Sleeper(0ms), Sleeper(0ms)};
  rex::platform::Wait(threads[0]->GetThread(), false);
  rex::platform::Wait(threads[1]->GetThread(), false);
  std::atomic<int> reports{0};
  rex::runtime::WaitForThreadsToStop(threads, 5s, [&](const auto&) { ++reports; });
  CHECK(reports.load() == 0);
}
