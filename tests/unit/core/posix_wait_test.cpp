/**
 * @file        tests/unit/core/posix_wait_test.cpp
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
#include <memory>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#endif

#include <rex/platform/thread.h>

using namespace std::chrono_literals;
using rex::platform::Event;
using rex::platform::Mutant;
using rex::platform::Semaphore;
using rex::platform::Thread;
using rex::platform::WaitHandle;
using rex::platform::WaitResult;

namespace {

using Clock = std::chrono::steady_clock;

struct ThreadUsage {
  long voluntary_switches = 0;
  std::chrono::microseconds cpu{0};
};

ThreadUsage CurrentThreadUsage() {
#if defined(__linux__)
  rusage usage{};
  getrusage(RUSAGE_THREAD, &usage);
  const auto micros = [](const timeval& t) {
    return std::chrono::seconds(t.tv_sec) + std::chrono::microseconds(t.tv_usec);
  };
  return {usage.ru_nvcsw, std::chrono::duration_cast<std::chrono::microseconds>(
                              micros(usage.ru_utime) + micros(usage.ru_stime))};
#else
  return {};
#endif
}

template <typename Predicate>
bool WaitUntil(Predicate ready, std::chrono::milliseconds limit = 5s) {
  constexpr int kYields = 256;
  const auto deadline = Clock::now() + limit;
  for (int attempt = 0; !ready(); ++attempt) {
    if (Clock::now() >= deadline) {
      return false;
    }
    if (attempt < kYields) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(100us);
    }
  }
  return true;
}

}  // namespace

REX_TEST_CASE("A multi wait on a plain thread sleeps until an event is set", "[core][posix_wait]") {
  auto first = Event::CreateManualResetEvent(false);
  auto second = Event::CreateManualResetEvent(false);
  ThreadUsage spent;
  Clock::duration latency{};
  std::atomic<Clock::rep> set_at{0};
  std::thread waiter([&] {
    WaitHandle* handles[2] = {first.get(), second.get()};
    const ThreadUsage before = CurrentThreadUsage();
    const auto result = rex::platform::WaitAny(handles, 2, false);
    latency = Clock::now() - Clock::time_point(Clock::duration(set_at.load()));
    const ThreadUsage after = CurrentThreadUsage();
    spent.voluntary_switches = after.voluntary_switches - before.voluntary_switches;
    REQUIRE(result.first == WaitResult::kSuccess);
    REQUIRE(result.second == 1);
  });
  std::this_thread::sleep_for(500ms);
  set_at = Clock::now().time_since_epoch().count();
  second->Set();
  waiter.join();
#if defined(__linux__)
  REQUIRE(spent.voluntary_switches < 10);
#endif
  REQUIRE(latency < 50ms);
}

REX_TEST_CASE("An alertable multi wait on a plain thread uses no CPU while idle",
              "[core][posix_wait]") {
  auto first = Event::CreateManualResetEvent(false);
  auto second = Event::CreateManualResetEvent(false);
  ThreadUsage spent;
  std::thread waiter([&] {
    WaitHandle* handles[2] = {first.get(), second.get()};
    const ThreadUsage before = CurrentThreadUsage();
    const auto result = rex::platform::WaitAny(handles, 2, true);
    const ThreadUsage after = CurrentThreadUsage();
    spent.cpu = after.cpu - before.cpu;
    spent.voluntary_switches = after.voluntary_switches - before.voluntary_switches;
    REQUIRE(result.first == WaitResult::kSuccess);
  });
  std::this_thread::sleep_for(500ms);
  first->Set();
  waiter.join();
#if defined(__linux__)
  REQUIRE(spent.cpu < 50ms);
  REQUIRE(spent.voluntary_switches < 10);
#endif
}

REX_TEST_CASE("A wait all ends only after both events are set", "[core][posix_wait]") {
  for (const bool first_then_second : {true, false}) {
    auto first = Event::CreateManualResetEvent(false);
    auto second = Event::CreateManualResetEvent(false);
    std::atomic<bool> done{false};
    std::thread waiter([&] {
      WaitHandle* handles[2] = {first.get(), second.get()};
      REQUIRE(rex::platform::WaitAll(handles, 2, false) == WaitResult::kSuccess);
      done = true;
    });
    (first_then_second ? first : second)->Set();
    std::this_thread::sleep_for(50ms);
    REQUIRE_FALSE(done.load());
    (first_then_second ? second : first)->Set();
    waiter.join();
    REQUIRE(done.load());
  }
}

REX_TEST_CASE("A wait any reports the index of an event set before the wait began",
              "[core][posix_wait]") {
  auto first = Event::CreateManualResetEvent(false);
  auto second = Event::CreateManualResetEvent(false);
  auto third = Event::CreateAutoResetEvent(false);
  third->Set();
  WaitHandle* handles[3] = {first.get(), second.get(), third.get()};
  const auto result = rex::platform::WaitAny(handles, 3, false, 1s);
  REQUIRE(result.first == WaitResult::kSuccess);
  REQUIRE(result.second == 2);
  REQUIRE(rex::platform::WaitAny(handles, 3, false, 20ms).first == WaitResult::kTimeout);
}

REX_TEST_CASE("A timed multi wait ends at its deadline", "[core][posix_wait]") {
  auto first = Event::CreateManualResetEvent(false);
  auto second = Event::CreateManualResetEvent(false);
  WaitHandle* handles[2] = {first.get(), second.get()};
  for (const bool alertable : {false, true}) {
    const auto start = Clock::now();
    const auto result = rex::platform::WaitAny(handles, 2, alertable, 100ms);
    const auto elapsed = Clock::now() - start;
    REQUIRE(result.first == WaitResult::kTimeout);
    REQUIRE(elapsed >= 100ms);
    REQUIRE(elapsed < 150ms);
  }
}

REX_TEST_CASE("A timed wait never returns before its deadline", "[core][posix_wait]") {
  auto event = Event::CreateManualResetEvent(false);
  auto other = Event::CreateManualResetEvent(false);
  WaitHandle* handles[2] = {event.get(), other.get()};
  for (const auto duration : {1ms, 7ms, 16ms, 37ms}) {
    for (int round = 0; round < 10; ++round) {
      for (const bool alertable : {false, true}) {
        auto start = Clock::now();
        REQUIRE(rex::platform::Wait(event.get(), alertable, duration) == WaitResult::kTimeout);
        REQUIRE(Clock::now() - start >= duration);
        start = Clock::now();
        REQUIRE(rex::platform::WaitAny(handles, 2, alertable, duration).first ==
                WaitResult::kTimeout);
        REQUIRE(Clock::now() - start >= duration);
        start = Clock::now();
        REQUIRE(rex::platform::WaitAll(handles, 2, alertable, duration) == WaitResult::kTimeout);
        REQUIRE(Clock::now() - start >= duration);
      }
    }
  }
}

REX_TEST_CASE("A queued callback ends every kind of alertable wait", "[core][posix_wait]") {
  auto never = Event::CreateManualResetEvent(false);
  auto other = Event::CreateManualResetEvent(false);
  auto signal = Event::CreateManualResetEvent(false);
  std::atomic<int> calls{0};
  std::atomic<int> stage{0};
  std::vector<WaitResult> results;
  std::latch started(1);
  auto thread = Thread::Create({}, [&] {
    started.count_down();
    WaitHandle* handles[2] = {never.get(), other.get()};
    stage = 1;
    results.push_back(rex::platform::Wait(never.get(), true));
    stage = 2;
    results.push_back(rex::platform::WaitAny(handles, 2, true).first);
    stage = 3;
    results.push_back(rex::platform::SignalAndWait(signal.get(), never.get(), true));
    stage = 4;
  });
  REQUIRE(thread != nullptr);
  started.wait();
  for (int expected = 1; expected <= 3; ++expected) {
    REQUIRE(WaitUntil([&] { return stage.load() == expected; }));
    std::this_thread::sleep_for(20ms);
    const auto queued = Clock::now();
    thread->QueueUserCallback([&] { ++calls; });
    REQUIRE(WaitUntil([&] { return stage.load() == expected + 1; }, 1s));
    REQUIRE(Clock::now() - queued < 100ms);
  }
  REQUIRE(rex::platform::Wait(thread.get(), false) == WaitResult::kSuccess);
  REQUIRE(calls.load() == 3);
  REQUIRE(results == std::vector<WaitResult>(3, WaitResult::kUserCallback));
}

REX_TEST_CASE("A callback that waits does not disturb the alertable wait that ran it",
              "[core][posix_wait]") {
  auto first = Event::CreateManualResetEvent(false);
  auto second = Event::CreateManualResetEvent(false);
  auto inner_first = Event::CreateManualResetEvent(true);
  auto inner_second = Event::CreateManualResetEvent(false);
  std::atomic<int> inner{-1};
  std::atomic<int> outer{-1};
  std::atomic<int> after{-1};
  std::atomic<bool> waiting{false};
  auto thread = Thread::Create({}, [&] {
    WaitHandle* handles[2] = {first.get(), second.get()};
    waiting = true;
    outer = static_cast<int>(rex::platform::WaitAny(handles, 2, true).first);
    after = static_cast<int>(rex::platform::WaitAny(handles, 2, true).first);
  });
  REQUIRE(thread != nullptr);
  REQUIRE(WaitUntil([&] { return waiting.load(); }));
  std::this_thread::sleep_for(20ms);
  thread->QueueUserCallback([&] {
    WaitHandle* handles[2] = {inner_first.get(), inner_second.get()};
    const auto result = rex::platform::WaitAny(handles, 2, false, 1s);
    inner = static_cast<int>(result.first) * 10 + static_cast<int>(result.second);
  });
  REQUIRE(WaitUntil([&] { return outer.load() != -1; }, 1s));
  REQUIRE(outer.load() == static_cast<int>(WaitResult::kUserCallback));
  REQUIRE(inner.load() == static_cast<int>(WaitResult::kSuccess) * 10);
  std::this_thread::sleep_for(20ms);
  second->Set();
  REQUIRE(rex::platform::Wait(thread.get(), false) == WaitResult::kSuccess);
  REQUIRE(after.load() == static_cast<int>(WaitResult::kSuccess));
}

REX_TEST_CASE("An abandoned mutant ends a multi wait with its index", "[core][posix_wait]") {
  std::unique_ptr<Mutant> mutant;
  auto thread = Thread::Create({}, [&] { mutant = Mutant::Create(true); });
  REQUIRE(thread != nullptr);
  REQUIRE(rex::platform::Wait(thread.get(), false) == WaitResult::kSuccess);
  auto event = Event::CreateManualResetEvent(false);
  WaitHandle* handles[2] = {mutant.get(), event.get()};
  const auto result = rex::platform::WaitAny(handles, 2, false, 1s);
  REQUIRE(result.first == WaitResult::kAbandoned);
  REQUIRE(result.second == 0);
}

REX_TEST_CASE("One release lets exactly one multi waiter through a semaphore",
              "[core][posix_wait]") {
  auto semaphore = Semaphore::Create(0, 10);
  auto never = Event::CreateManualResetEvent(false);
  std::atomic<int> through{0};
  std::vector<std::thread> waiters;
  waiters.reserve(2);
  for (int i = 0; i < 2; ++i) {
    waiters.emplace_back([&] {
      WaitHandle* handles[2] = {semaphore.get(), never.get()};
      if (rex::platform::WaitAny(handles, 2, false).first == WaitResult::kSuccess) {
        ++through;
      }
    });
  }
  std::this_thread::sleep_for(50ms);
  REQUIRE(semaphore->Release(1, nullptr));
  REQUIRE(WaitUntil([&] { return through.load() == 1; }));
  std::this_thread::sleep_for(100ms);
  REQUIRE(through.load() == 1);
  REQUIRE(semaphore->Release(1, nullptr));
  for (auto& waiter : waiters) {
    waiter.join();
  }
  REQUIRE(through.load() == 2);
}

REX_TEST_CASE("Many multi waiters lose no wakeup under contention", "[core][posix_wait]") {
  constexpr int kEvents = 4;
  constexpr int kWaiters = 8;
  constexpr int kRounds = 10000;
  std::vector<std::unique_ptr<Event>> events;
  events.reserve(kEvents);
  for (int i = 0; i < kEvents; ++i) {
    events.push_back(Event::CreateAutoResetEvent(false));
  }
  std::atomic<int> consumed{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> waiters;
  waiters.reserve(kWaiters);
  for (int i = 0; i < kWaiters; ++i) {
    waiters.emplace_back([&] {
      std::vector<WaitHandle*> handles;
      handles.reserve(events.size());
      for (auto& event : events) {
        handles.push_back(event.get());
      }
      while (!stop) {
        if (rex::platform::WaitAny(handles, false, 50ms).first == WaitResult::kSuccess) {
          ++consumed;
        }
      }
    });
  }
  bool lost = false;
  for (int round = 0; round < kRounds && !lost; ++round) {
    events[round % kEvents]->Set();
    lost = !WaitUntil([&] { return consumed.load() == round + 1; }, 5s);
  }
  stop = true;
  for (auto& waiter : waiters) {
    waiter.join();
  }
  REQUIRE_FALSE(lost);
  REQUIRE(consumed.load() == kRounds);
}
