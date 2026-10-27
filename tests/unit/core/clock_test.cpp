/**
 * @file        tests/unit/core/clock_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/chrono/clock.h>

namespace {

struct EarlyClockReads {
  rex::u64 tick_frequency;
  rex::u64 system_time;
  rex::u64 tick_count;

  EarlyClockReads()
      : tick_frequency(rex::Clock::GetGuestTickFrequency()),
        system_time(rex::Clock::QueryGuestSystemTime()),
        tick_count(rex::Clock::QueryGuestTickCount()) {}
};

const EarlyClockReads g_early_reads;

}  // namespace

REX_TEST_CASE("Clock globals are constant initialized", "[core][clock]") {
  CHECK(g_early_reads.tick_frequency == rex::Clock::GetHostTickFrequency());
  CHECK(g_early_reads.system_time != 0);
  CHECK(g_early_reads.tick_count < rex::Clock::GetHostTickFrequency());
}

REX_TEST_CASE("Guest system time tracks the host after the setters run", "[core][clock]") {
  const rex::u64 frequency = rex::Clock::GetGuestTickFrequency();
  const rex::u64 base = rex::Clock::GetGuestSystemTimeBase();
  rex::Clock::SetGuestTickFrequency(50000000);
  rex::Clock::SetGuestSystemTimeBase(rex::Clock::QueryHostSystemTime());
  CHECK(rex::Clock::GetGuestTickFrequency() == 50000000);
  CHECK(rex::Clock::GetGuestSystemTimeBase() != 0);
  rex::Clock::SetGuestTickFrequency(frequency);
  rex::Clock::SetGuestSystemTimeBase(base);
}
