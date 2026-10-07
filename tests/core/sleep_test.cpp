/**
 * @file        tests/core/sleep_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include <rex/platform/thread.h>

namespace {

std::chrono::microseconds TimedSleep(std::chrono::microseconds duration) {
  const auto start = std::chrono::steady_clock::now();
  rex::platform::Sleep(duration);
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                               start);
}

}  // namespace

TEST_CASE("Sleep never returns before a sub millisecond duration", "[core][sleep]") {
  for (int i = 0; i < 20; ++i) {
    CHECK(TimedSleep(std::chrono::microseconds(500)) >= std::chrono::microseconds(500));
  }
}

TEST_CASE("Sleep never returns before a multi millisecond duration", "[core][sleep]") {
  for (int i = 0; i < 10; ++i) {
    CHECK(TimedSleep(std::chrono::microseconds(3500)) >= std::chrono::microseconds(3500));
  }
}

TEST_CASE("Sleep stays near a multi millisecond duration at the default timer period",
          "[core][sleep]") {
  std::chrono::microseconds total{0};
  for (int i = 0; i < 20; ++i) {
    total += TimedSleep(std::chrono::microseconds(3000));
  }
  CHECK(total / 20 < std::chrono::microseconds(8000));
}
