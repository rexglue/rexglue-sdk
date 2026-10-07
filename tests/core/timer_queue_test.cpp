/**
 * @file        tests/core/timer_queue_test.cpp
 * @brief       Dispatch order and disarm of the process timer queue
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
#include <mutex>
#include <thread>
#include <vector>

#include <rex/platform/thread.h>
#include <rex/platform/timer_queue.h>
#include <rex/types.h>

namespace {

using namespace std::chrono_literals;
using Clock = rex::platform::TimerQueueWaitItem::clock;

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = 2000ms) {
  const auto deadline = Clock::now() + timeout;
  while (!predicate()) {
    if (Clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

struct QueueReset : public rex::NonCopyable {
  QueueReset() = default;
  ~QueueReset() { rex::platform::detail::ShutdownTimerQueue(); }
};

}  // namespace

TEST_CASE("A once timer fires once", "[core][timer_queue]") {
  QueueReset reset;
  std::atomic<int> calls{0};
  auto item = rex::platform::QueueTimerOnce([&](void*) { calls.fetch_add(1); }, nullptr,
                                            Clock::now() + 10ms);
  REQUIRE(WaitFor([&] { return calls.load() == 1; }));
  std::this_thread::sleep_for(50ms);
  CHECK(calls.load() == 1);
}

TEST_CASE("A recurring timer repeats until disarmed", "[core][timer_queue]") {
  QueueReset reset;
  std::atomic<int> calls{0};
  auto item = rex::platform::QueueTimerRecurring([&](void*) { calls.fetch_add(1); }, nullptr,
                                                 Clock::now() + 5ms, 5ms);
  REQUIRE(WaitFor([&] { return calls.load() >= 3; }));
  auto locked = item.lock();
  REQUIRE(locked);
  locked->Disarm();
  const int after_disarm = calls.load();
  std::this_thread::sleep_for(50ms);
  CHECK(calls.load() == after_disarm);
}

TEST_CASE("Disarm before the due time prevents the call", "[core][timer_queue]") {
  QueueReset reset;
  std::atomic<int> calls{0};
  auto item = rex::platform::QueueTimerOnce([&](void*) { calls.fetch_add(1); }, nullptr,
                                            Clock::now() + 200ms);
  auto locked = item.lock();
  REQUIRE(locked);
  locked->Disarm();
  std::this_thread::sleep_for(300ms);
  CHECK(calls.load() == 0);
}

TEST_CASE("A recurring timer disarms itself from its callback", "[core][timer_queue]") {
  QueueReset reset;
  std::atomic<int> calls{0};
  std::mutex handle_mutex;
  std::weak_ptr<rex::platform::TimerQueueWaitItem> handle;
  auto callback = [&](void*) {
    if (calls.fetch_add(1) + 1 == 2) {
      std::lock_guard lock(handle_mutex);
      if (auto self = handle.lock()) {
        self->Disarm();
      }
    }
  };
  {
    std::lock_guard lock(handle_mutex);
    handle = rex::platform::QueueTimerRecurring(callback, nullptr, Clock::now() + 50ms, 5ms);
  }
  REQUIRE(WaitFor([&] { return calls.load() >= 2; }));
  std::this_thread::sleep_for(50ms);
  CHECK(calls.load() == 2);
}

TEST_CASE("Disarm waits for a running callback", "[core][timer_queue]") {
  QueueReset reset;
  std::atomic<bool> entered{false};
  std::atomic<bool> finished{false};
  auto item = rex::platform::QueueTimerOnce(
      [&](void*) {
        entered.store(true);
        std::this_thread::sleep_for(200ms);
        finished.store(true);
      },
      nullptr, Clock::now());
  REQUIRE(WaitFor([&] { return entered.load(); }));
  auto locked = item.lock();
  REQUIRE(locked);
  locked->Disarm();
  CHECK(finished.load());
}

TEST_CASE("Timers fire in due order", "[core][timer_queue]") {
  QueueReset reset;
  std::mutex order_mutex;
  std::vector<int> order;
  auto record = [&](int id) {
    std::lock_guard lock(order_mutex);
    order.push_back(id);
  };
  const auto now = Clock::now();
  auto late = rex::platform::QueueTimerOnce([&](void*) { record(2); }, nullptr, now + 60ms);
  auto early = rex::platform::QueueTimerOnce([&](void*) { record(1); }, nullptr, now + 30ms);
  REQUIRE(WaitFor([&] {
    std::lock_guard lock(order_mutex);
    return order.size() == 2;
  }));
  std::lock_guard lock(order_mutex);
  CHECK(order == std::vector<int>{1, 2});
}

TEST_CASE("An earlier timer queued later wakes the dispatch thread", "[core][timer_queue]") {
  QueueReset reset;
  std::atomic<int> calls{0};
  auto distant = rex::platform::QueueTimerOnce([](void*) {}, nullptr, Clock::now() + 1h);
  std::this_thread::sleep_for(20ms);
  const auto queued = Clock::now();
  auto soon =
      rex::platform::QueueTimerOnce([&](void*) { calls.fetch_add(1); }, nullptr, queued + 10ms);
  REQUIRE(WaitFor([&] { return calls.load() == 1; }, 500ms));
  CHECK(Clock::now() - queued < 400ms);
}

TEST_CASE("Shutdown with pending timers returns promptly", "[core][timer_queue]") {
  QueueReset reset;
  auto item = rex::platform::QueueTimerOnce([](void*) {}, nullptr, Clock::now() + 1h);
  const auto start = Clock::now();
  rex::platform::detail::ShutdownTimerQueue();
  CHECK(Clock::now() - start < 1s);
}
