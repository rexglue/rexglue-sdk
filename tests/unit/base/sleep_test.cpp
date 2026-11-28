/**
 * @file        tests/unit/base/sleep_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <chrono>

#include <rex/platform.h>
#include <rex/platform/thread.h>

#if REX_PLATFORM_WIN32
#include "base/sleep_state.h"
#endif

namespace {

std::chrono::microseconds TimedSleep(std::chrono::microseconds duration) {
  const auto start = std::chrono::steady_clock::now();
  rex::platform::SleepFor(duration);
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                               start);
}

}  // namespace

// Proves a sub millisecond sleep, which is mostly the spin tail, never returns early.
REX_TEST_CASE("Sleep never returns before a sub millisecond duration", "[base][sleep]") {
  for (int i = 0; i < 20; ++i) {
    CHECK(TimedSleep(std::chrono::microseconds(500)) >= std::chrono::microseconds(500));
  }
}

// Proves a multi millisecond sleep, which waits on the timer then spins, never returns early.
REX_TEST_CASE("Sleep never returns before a multi millisecond duration", "[base][sleep]") {
  for (int i = 0; i < 10; ++i) {
    CHECK(TimedSleep(std::chrono::microseconds(3500)) >= std::chrono::microseconds(3500));
  }
}

#if REX_PLATFORM_WIN32
// Proves SleepFor reached its high resolution timer every time, since a fallback to the system
// timer period is counted, and that no sleep returned early.
REX_TEST_CASE("Sleep uses the high resolution timer for a multi millisecond duration",
              "[base][sleep]") {
  const u64 fallbacks = rex::platform::detail::SleepTimerFallbackCount();
  for (int i = 0; i < 21; ++i) {
    CHECK(TimedSleep(std::chrono::microseconds(3000)) >= std::chrono::microseconds(3000));
  }
  CHECK(rex::platform::detail::SleepTimerFallbackCount() == fallbacks);
}
#endif
