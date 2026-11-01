/**
 * @file        tests/unit/base/timer_queue_test.cpp
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
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/platform/thread.h>
#include <rex/platform/timer.h>
#include <rex/types.h>
#include <rex/utility.h>

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

bool WaitForSentinel(Clock::time_point due) {
  std::atomic<bool> fired{false};
  auto sentinel = rex::platform::QueueTimerOnce([&](void*) { fired.store(true); }, nullptr, due);
  return WaitFor([&] { return fired.load(); });
}

struct QueueReset : public rex::NonCopyable {
  QueueReset() = default;
  ~QueueReset() { rex::platform::detail::ShutdownTimerQueue(); }
};

}  // namespace

REX_TEST_CASE("A once timer fires once", "[base][timer_queue]") {
  QueueReset reset;
  std::atomic<int> calls{0};
  auto item = rex::platform::QueueTimerOnce([&](void*) { calls.fetch_add(1); }, nullptr,
                                            Clock::now() + 10ms);
  REQUIRE(WaitFor([&] { return calls.load() == 1; }));
  REQUIRE(WaitForSentinel(Clock::now() + 30ms));
  CHECK(calls.load() == 1);
}

REX_TEST_CASE("A recurring timer repeats until disarmed", "[base][timer_queue]") {
  QueueReset reset;
  std::atomic<int> calls{0};
  auto item = rex::platform::QueueTimerRecurring([&](void*) { calls.fetch_add(1); }, nullptr,
                                                 Clock::now() + 5ms, 5ms);
  REQUIRE(WaitFor([&] { return calls.load() >= 3; }));
  auto locked = item.lock();
  REQUIRE(locked);
  locked->Disarm();
  const int after_disarm = calls.load();
  REQUIRE(WaitForSentinel(Clock::now() + 30ms));
  CHECK(calls.load() == after_disarm);
}

REX_TEST_CASE("Disarm before the due time prevents the call", "[base][timer_queue]") {
  QueueReset reset;
  std::atomic<int> calls{0};
  auto item = rex::platform::QueueTimerOnce([&](void*) { calls.fetch_add(1); }, nullptr,
                                            Clock::now() + 200ms);
  auto locked = item.lock();
  REQUIRE(locked);
  locked->Disarm();
  REQUIRE(WaitForSentinel(Clock::now() + 250ms));
  CHECK(calls.load() == 0);
}

REX_TEST_CASE("A recurring timer disarms itself from its callback", "[base][timer_queue]") {
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
  REQUIRE(WaitForSentinel(Clock::now() + 30ms));
  CHECK(calls.load() == 2);
}

REX_TEST_CASE("Disarm waits for a running callback", "[base][timer_queue]") {
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

REX_TEST_CASE("Timers fire in due order", "[base][timer_queue]") {
  QueueReset reset;
  std::mutex order_mutex;
  std::vector<int> order;
  auto append_order = [&](int id) {
    std::lock_guard lock(order_mutex);
    order.push_back(id);
  };
  const auto now = Clock::now();
  auto late = rex::platform::QueueTimerOnce([&](void*) { append_order(2); }, nullptr, now + 60ms);
  auto early = rex::platform::QueueTimerOnce([&](void*) { append_order(1); }, nullptr, now + 30ms);
  REQUIRE(WaitFor([&] {
    std::lock_guard lock(order_mutex);
    return order.size() == 2;
  }));
  std::lock_guard lock(order_mutex);
  CHECK(order == std::vector<int>{1, 2});
}

REX_TEST_CASE("An earlier timer queued later wakes the dispatch thread", "[base][timer_queue]") {
  QueueReset reset;
  std::atomic<int> calls{0};
  auto distant = rex::platform::QueueTimerOnce([](void*) {}, nullptr, Clock::now() + 1h);
  std::this_thread::sleep_for(20ms);
  auto soon = rex::platform::QueueTimerOnce([&](void*) { calls.fetch_add(1); }, nullptr,
                                            Clock::now() + 10ms);
  REQUIRE(WaitFor([&] { return calls.load() == 1; }, 500ms));
}

REX_TEST_CASE("Shutdown returns with pending timers", "[base][timer_queue]") {
  QueueReset reset;
  auto item = rex::platform::QueueTimerOnce([](void*) {}, nullptr, Clock::now() + 1h);
  rex::platform::detail::ShutdownTimerQueue();
}
