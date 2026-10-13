/**
 * @file        tests/unit/core/thread_start_data_test.cpp
 * @brief       Start data handed to a host thread runs once and is freed once
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
#include <thread>

#include <rex/platform/thread.h>

namespace {

using namespace std::chrono_literals;

struct Token {
  explicit Token(std::atomic<int>& freed_in) : freed(freed_in) {}
  ~Token() { ++freed; }
  std::atomic<int>& freed;
};

}  // namespace

REX_TEST_CASE("Thread start data runs once and is freed once", "[core][thread]") {
  std::atomic<int> ran{0};
  std::atomic<int> freed{0};
  {
    auto token = std::make_shared<Token>(freed);
    auto thread = rex::platform::Thread::Create({}, [token, &ran] { ++ran; });
    REQUIRE(thread != nullptr);
    token.reset();
    REQUIRE(rex::platform::Wait(thread.get(), false) == rex::platform::WaitResult::kSuccess);
    for (int i = 0; i < 500 && freed.load() == 0; ++i) {
      std::this_thread::sleep_for(2ms);
    }
  }
  std::this_thread::sleep_for(20ms);
  CHECK(ran.load() == 1);
  CHECK(freed.load() == 1);
}
