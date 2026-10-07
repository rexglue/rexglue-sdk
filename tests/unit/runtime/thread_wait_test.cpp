/**
 * @file        tests/unit/system/thread_wait_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <vector>

#include "runtime/thread_wait.h"

namespace {

struct FakeThread {
  bool running = false;
  // NOLINTNEXTLINE(readability-identifier-naming): the ThreadRecord spelling the helper calls
  bool is_running() const { return running; }
};

using Clock = std::chrono::steady_clock;

}  // namespace

TEST_CASE("A running thread holds the wait to its bound", "[thread_wait]") {
  std::vector<std::shared_ptr<FakeThread>> threads{std::make_shared<FakeThread>(),
                                                   std::make_shared<FakeThread>(FakeThread{true})};
  const auto start = Clock::now();
  REQUIRE_FALSE(rex::runtime::WaitForThreadsToExit(threads, std::chrono::milliseconds(50)));
  const auto elapsed = Clock::now() - start;
  REQUIRE(elapsed >= std::chrono::milliseconds(50));
  REQUIRE(elapsed < std::chrono::seconds(1));
}

TEST_CASE("Stopped threads end the wait at once", "[thread_wait]") {
  std::vector<std::shared_ptr<FakeThread>> threads{std::make_shared<FakeThread>(),
                                                   std::make_shared<FakeThread>()};
  const auto start = Clock::now();
  REQUIRE(rex::runtime::WaitForThreadsToExit(threads, std::chrono::seconds(5)));
  REQUIRE(Clock::now() - start < std::chrono::milliseconds(50));
}
