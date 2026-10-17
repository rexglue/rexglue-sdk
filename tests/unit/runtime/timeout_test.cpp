/**
 * @file        tests/unit/runtime/timeout_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <chrono>

#include <rex/runtime/object.h>

using namespace std::chrono_literals;

REX_TEST_CASE("A zero timeout stays a poll", "[runtime][timeout]") {
  REQUIRE(rex::runtime::TimeoutFromTicks(0) == 0ms);
}

REX_TEST_CASE("A relative timeout rounds up to a whole millisecond", "[runtime][timeout]") {
  REQUIRE(rex::runtime::TimeoutFromTicks(-1) == 1ms);
  REQUIRE(rex::runtime::TimeoutFromTicks(-5000) == 1ms);
  REQUIRE(rex::runtime::TimeoutFromTicks(-10000) == 1ms);
  REQUIRE(rex::runtime::TimeoutFromTicks(-10001) == 2ms);
  REQUIRE(rex::runtime::TimeoutFromTicks(-1000000) == 100ms);
}

REX_TEST_CASE("A delay interval keeps its precision below a millisecond", "[runtime][timeout]") {
  using namespace std::chrono_literals;
  REQUIRE(rex::runtime::DelayFromTicks(0) == 0us);
  REQUIRE(rex::runtime::DelayFromTicks(-1) == 1us);
  REQUIRE(rex::runtime::DelayFromTicks(-10) == 1us);
  REQUIRE(rex::runtime::DelayFromTicks(-11) == 2us);
  REQUIRE(rex::runtime::DelayFromTicks(-5000) == 500us);
  REQUIRE(rex::runtime::DelayFromTicks(-15000) == 1500us);
}
