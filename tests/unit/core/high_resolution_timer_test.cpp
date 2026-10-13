/**
 * @file        tests/unit/core/high_resolution_timer_test.cpp
 * @brief       HighResolutionTimer is built only by CreateRepeating
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
#include <functional>
#include <thread>
#include <type_traits>

#include <rex/platform/thread.h>
#include <rex/platform/timer_queue.h>

namespace {

using namespace std::chrono_literals;
using rex::platform::HighResolutionTimer;

static_assert(!std::is_default_constructible_v<HighResolutionTimer>);
static_assert(!std::is_constructible_v<HighResolutionTimer, std::chrono::milliseconds,
                                       std::function<void()>>);

struct QueueReset : public rex::NonCopyable {
  QueueReset() = default;
  ~QueueReset() { rex::platform::detail::ShutdownTimerQueue(); }
};

}  // namespace

REX_TEST_CASE("CreateRepeating returns a running timer built through the key", "[core][timer]") {
  QueueReset reset;
  std::atomic<int> ticks{0};
  auto timer = HighResolutionTimer::CreateRepeating(2ms, [&ticks] { ++ticks; });
  REQUIRE(timer != nullptr);
  for (int i = 0; i < 500 && ticks.load() < 2; ++i) {
    std::this_thread::sleep_for(2ms);
  }
  CHECK(ticks.load() >= 2);
}
