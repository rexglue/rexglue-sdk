/**
 * @file        tests/unit/core/thread_time_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <chrono>

#include <rex/platform/thread.h>
#include <rex/types.h>

REX_TEST_CASE("GetCurrentThreadCPUTime grows across a busy loop", "[core][thread_time]") {
  const auto start = rex::platform::GetCurrentThreadCPUTime();
  const auto cap = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  auto previous = start;
  auto spun = std::chrono::milliseconds::zero();
  bool monotonic = true;
  volatile rex::u64 counter = 0;
  while (spun < std::chrono::milliseconds(100) && std::chrono::steady_clock::now() < cap) {
    counter = counter + 1;
    const auto now = rex::platform::GetCurrentThreadCPUTime();
    monotonic = monotonic && now >= previous;
    previous = now;
    spun = now - start;
  }
  CHECK(monotonic);
  CHECK(spun >= std::chrono::milliseconds(100));
}

REX_TEST_CASE("GetCurrentThreadCPUTime stays flat across a sleep", "[core][thread_time]") {
  const auto start = rex::platform::GetCurrentThreadCPUTime();
  rex::platform::Sleep(std::chrono::milliseconds(100));
  CHECK(rex::platform::GetCurrentThreadCPUTime() - start < std::chrono::milliseconds(50));
}
