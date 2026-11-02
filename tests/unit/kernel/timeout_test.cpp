/**
 * @file        tests/unit/kernel/timeout_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <chrono>

#include <rex/kernel/object/object.h>

using namespace std::chrono_literals;

REX_TEST_CASE("A zero timeout stays a poll", "[kernel][timeout]") {
  REQUIRE(rex::kernel::TimeoutFromTicks(0) == 0ms);
}

REX_TEST_CASE("A relative timeout rounds up to a whole millisecond", "[kernel][timeout]") {
  REQUIRE(rex::kernel::TimeoutFromTicks(-1) == 1ms);
  REQUIRE(rex::kernel::TimeoutFromTicks(-5000) == 1ms);
  REQUIRE(rex::kernel::TimeoutFromTicks(-10000) == 1ms);
  REQUIRE(rex::kernel::TimeoutFromTicks(-10001) == 2ms);
  REQUIRE(rex::kernel::TimeoutFromTicks(-1000000) == 100ms);
}

REX_TEST_CASE("A delay interval keeps its precision below a millisecond", "[kernel][timeout]") {
  using namespace std::chrono_literals;
  REQUIRE(rex::kernel::DelayFromTicks(0) == 0us);
  REQUIRE(rex::kernel::DelayFromTicks(-1) == 1us);
  REQUIRE(rex::kernel::DelayFromTicks(-10) == 1us);
  REQUIRE(rex::kernel::DelayFromTicks(-11) == 2us);
  REQUIRE(rex::kernel::DelayFromTicks(-5000) == 500us);
  REQUIRE(rex::kernel::DelayFromTicks(-15000) == 1500us);
}
