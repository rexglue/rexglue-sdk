/**
 * @file        tests/unit/base/counter_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <cstring>
#include <deque>
#include <string>

#include <rex/platform/perf.h>

using rex::platform::CounterID;
using rex::platform::CounterType;
using rex::platform::FindCounter;
using rex::platform::RegisterCounter;

REX_TEST_CASE("The frame counters are registered first", "[base][perf]") {
  CHECK(FindCounter("frame_time_us") == CounterID::kFrameTimeUs);
  CHECK(FindCounter("fps") == CounterID::kFps);
  CHECK(std::strcmp(rex::platform::GetCounterName(CounterID::kFps), "fps") == 0);
}

REX_TEST_CASE("Registering a name again returns the same counter", "[base][perf]") {
  const auto first = RegisterCounter("test_counter_same");
  const auto second = RegisterCounter("test_counter_same", CounterType::kGauge);
  REQUIRE(first);
  REQUIRE(second);
  CHECK(*first == *second);
  CHECK(FindCounter("test_counter_same") == *first);
}

REX_TEST_CASE("An unknown name finds no counter", "[base][perf]") {
  CHECK_FALSE(FindCounter("test_counter_never_registered").has_value());
  CHECK(std::strcmp(rex::platform::GetCounterName(static_cast<CounterID>(255)), "unknown") == 0);
}

REX_TEST_CASE("A frame snapshot zeroes accumulators and keeps gauges", "[base][perf]") {
  const auto accumulator_result = RegisterCounter("test_counter_accumulator");
  const auto gauge_result = RegisterCounter("test_counter_gauge", CounterType::kGauge);
  REQUIRE(accumulator_result);
  REQUIRE(gauge_result);
  const CounterID accumulator = *accumulator_result;
  const CounterID gauge = *gauge_result;
  rex::platform::SetCounter(accumulator, 5);
  rex::platform::IncrementCounter(accumulator, 2);
  rex::platform::SetCounter(gauge, 9);
  rex::platform::ResetFrameCounters();
  CHECK(rex::platform::GetSnapshotCounter(accumulator) == 7);
  CHECK(rex::platform::GetSnapshotCounter(gauge) == 9);
  CHECK(rex::platform::GetCounter(accumulator) == 0);
  CHECK(rex::platform::GetCounter(gauge) == 9);
}

REX_TEST_CASE("A full counter table refuses a new name and still finds the others",
              "[base][perf]") {
  static std::deque<std::string> names;
  size_t registered = 0;
  for (; registered < rex::platform::kMaxCounters; ++registered) {
    names.push_back("test_counter_fill_" + std::to_string(registered));
    if (!RegisterCounter(names.back().c_str())) {
      break;
    }
  }
  REQUIRE(registered < rex::platform::kMaxCounters);
  names.push_back("test_counter_overflow");
  const auto refused = RegisterCounter(names.back().c_str());
  REQUIRE_FALSE(refused);
  CHECK(refused.error().message.find("test_counter_overflow") != std::string::npos);
  CHECK_FALSE(FindCounter("test_counter_overflow").has_value());
  CHECK(FindCounter("frame_time_us") == CounterID::kFrameTimeUs);
  const auto again = RegisterCounter(names.front().c_str());
  CHECK(again);
}
