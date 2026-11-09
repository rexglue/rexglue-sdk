/**
 * @file        tests/unit/base/thread_priority_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <thread>

#include <rex/platform/thread.h>
#include <rex/types.h>

REX_TEST_CASE("SetCurrentThreadPriority accepts every ThreadPriority value", "[base][thread]") {
  constexpr std::array<rex::i32, 5> kPriorities = {
      rex::platform::ThreadPriority::kLowest,  rex::platform::ThreadPriority::kBelowNormal,
      rex::platform::ThreadPriority::kNormal,  rex::platform::ThreadPriority::kAboveNormal,
      rex::platform::ThreadPriority::kHighest,
  };
  bool finished = false;
  std::thread worker([&] {
    for (const rex::i32 priority : kPriorities) {
      rex::platform::SetCurrentThreadPriority(priority);
    }
    rex::platform::SetCurrentThreadPriority(rex::platform::ThreadPriority::kNormal);
    finished = true;
  });
  worker.join();
  CHECK(finished);
}
