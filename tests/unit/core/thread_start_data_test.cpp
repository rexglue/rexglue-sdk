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
#include <latch>
#include <memory>

#include <rex/platform/thread.h>

namespace {

struct Token {
  Token(std::atomic<int>& freed_in, std::latch& done_in) : freed(freed_in), done(done_in) {}
  ~Token() {
    ++freed;
    done.count_down();
  }
  std::atomic<int>& freed;
  std::latch& done;
};

}  // namespace

REX_TEST_CASE("Thread start data runs once and is freed once", "[core][thread]") {
  std::atomic<int> ran{0};
  std::atomic<int> freed{0};
  std::latch done(1);
  {
    auto token = std::make_shared<Token>(freed, done);
    auto thread = rex::platform::Thread::Create({}, [token, &ran] { ++ran; });
    REQUIRE(thread != nullptr);
    token.reset();
    REQUIRE(rex::platform::Wait(thread.get(), false) == rex::platform::WaitResult::kSuccess);
    done.wait();
  }
  CHECK(ran.load() == 1);
  CHECK(freed.load() == 1);
}
