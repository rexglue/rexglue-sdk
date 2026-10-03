/**
 * @file        tests/core/thread_time_test.cpp
 * @brief       CPU time of the calling thread from the platform layer
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
#include <rex/types.h>

namespace {

void Spin(std::chrono::milliseconds duration) {
  const auto until = std::chrono::steady_clock::now() + duration;
  volatile rex::u64 counter = 0;
  while (std::chrono::steady_clock::now() < until) {
    counter = counter + 1;
  }
}

}  // namespace

TEST_CASE("QueryCurrentThreadCPUTime grows across a busy loop", "[core][thread_time]") {
  const auto start = rex::platform::QueryCurrentThreadCPUTime();
  Spin(std::chrono::milliseconds(150));
  CHECK(rex::platform::QueryCurrentThreadCPUTime() - start >= std::chrono::milliseconds(100));
}

TEST_CASE("QueryCurrentThreadCPUTime stays flat across a sleep", "[core][thread_time]") {
  const auto start = rex::platform::QueryCurrentThreadCPUTime();
  rex::platform::Sleep(std::chrono::milliseconds(100));
  CHECK(rex::platform::QueryCurrentThreadCPUTime() - start < std::chrono::milliseconds(50));
}

TEST_CASE("QueryCurrentThreadCPUTime reports its cost a call", "[core][thread_time]") {
  constexpr int kCalls = 100000;
  std::chrono::milliseconds sink{0};
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < kCalls; ++i) {
    sink += rex::platform::QueryCurrentThreadCPUTime();
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - start);
  WARN("QueryCurrentThreadCPUTime costs " << elapsed.count() / kCalls << " ns a call, sink "
                                          << sink.count());
  if (elapsed / kCalls >= std::chrono::microseconds(2)) {
    WARN("QueryCurrentThreadCPUTime costs 2 microseconds or more a call");
  }
}
