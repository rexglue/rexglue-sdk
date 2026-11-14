/**
 * @file        tests/unit/base/chrono_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <chrono>
#include <cstdint>
#include <thread>

#include "rex_test.h"

#include <rex/chrono.h>

using rex::WinSystemClock;

// All values are 100 nanosecond intervals since 1601-01-01 00:00:00 UTC.
static constexpr u64 kFtNtEpoch = 0;                    /**< UTC 1601-01-01 */
static constexpr u64 kFtUnixEpoch = 116444736000000000; /**< UTC 1970-01-01 */
static constexpr u64 kFtY2k = 125911584000000000;       /**< UTC 2000-01-01 */
static constexpr u64 kFtSubDay = 132538032123450000;    /**< UTC 2020-12-30 12:00:12.345 */
static constexpr u64 kFt2021 = 132539328000000000;      /**< UTC 2021-01-01 */

REX_TEST_CASE("FromFileTime and ToFileTime are exact inverses", "[base][chrono]") {
  SECTION("zero (NT epoch)") {
    auto tp = WinSystemClock::FromFileTime(kFtNtEpoch);
    CHECK(WinSystemClock::ToFileTime(tp) == kFtNtEpoch);
  }
  SECTION("Unix epoch") {
    auto tp = WinSystemClock::FromFileTime(kFtUnixEpoch);
    CHECK(WinSystemClock::ToFileTime(tp) == kFtUnixEpoch);
  }
  SECTION("large value") {
    // Near year 9999.
    constexpr u64 kLarge = 2650467743990000000ULL;
    auto tp = WinSystemClock::FromFileTime(kLarge);
    CHECK(WinSystemClock::ToFileTime(tp) == kLarge);
  }
}

REX_TEST_CASE("ToFileTime then FromFileTime round trips", "[base][chrono]") {
  auto tp = WinSystemClock::FromFileTime(kFtSubDay);
  auto ft = WinSystemClock::ToFileTime(tp);
  auto tp2 = WinSystemClock::FromFileTime(ft);
  CHECK(tp == tp2);
}

REX_TEST_CASE("to_sys converts known FILETIMEs to correct system_clock values", "[base][chrono]") {
  using namespace std::chrono;

  SECTION("Unix epoch FILETIME -> system_clock epoch") {
    auto tp = WinSystemClock::to_sys(WinSystemClock::FromFileTime(kFtUnixEpoch));
    auto since_epoch = tp.time_since_epoch();
    CHECK(duration_cast<seconds>(since_epoch).count() == 0);
  }
  SECTION("Y2K FILETIME -> 30 years after Unix epoch") {
    auto tp = WinSystemClock::to_sys(WinSystemClock::FromFileTime(kFtY2k));
    auto since_epoch = tp.time_since_epoch();
    // 2000-01-01 is 10957 days after 1970-01-01
    CHECK(duration_cast<seconds>(since_epoch).count() == 10957LL * 86400);
  }
  SECTION("2021 FILETIME -> known seconds since Unix epoch") {
    auto tp = WinSystemClock::to_sys(WinSystemClock::FromFileTime(kFt2021));
    auto since_epoch = tp.time_since_epoch();
    // 2021-01-01 is 18628 days after 1970-01-01
    CHECK(duration_cast<seconds>(since_epoch).count() == 18628LL * 86400);
  }
}

REX_TEST_CASE("from_sys then to_sys round trips for whole second values", "[base][chrono]") {
  for (u64 ft : {kFtNtEpoch, kFtUnixEpoch, kFtY2k, kFt2021}) {
    auto nt_tp = WinSystemClock::FromFileTime(ft);
    auto sys_tp = WinSystemClock::to_sys(nt_tp);
    auto nt_tp2 = WinSystemClock::from_sys(sys_tp);
    auto ft2 = WinSystemClock::ToFileTime(nt_tp2);
    CHECK(ft2 == ft);
  }
}

REX_TEST_CASE("The host tick count advances at the rate of steady_clock", "[base][chrono]") {
  const auto start = std::chrono::steady_clock::now();
  const rex::u64 ticks_start = rex::Clock::GetHostTickCount();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  const rex::u64 ticks = rex::Clock::GetHostTickCount() - ticks_start;
  const auto elapsed = std::chrono::duration<rex::f64>(std::chrono::steady_clock::now() - start);
  const rex::f64 seconds =
      static_cast<rex::f64>(ticks) / static_cast<rex::f64>(rex::Clock::GetHostTickFrequency());
  CHECK(seconds / elapsed.count() > 0.99);
  CHECK(seconds / elapsed.count() < 1.01);
}

REX_TEST_CASE("A scaled relative file time stays negative", "[base][chrono]") {
  const rex::f64 scalar = rex::Clock::GetGuestTimeScalar();
  rex::Clock::SetGuestTimeScalar(0.5);
  CHECK(rex::Clock::ScaleGuestDurationFileTime(-10'000'000) == -5'000'000);
  rex::Clock::SetGuestTimeScalar(2.0);
  CHECK(rex::Clock::ScaleGuestDurationFileTime(-10'000'000) == -20'000'000);
  rex::Clock::SetGuestTimeScalar(scalar);
}

REX_TEST_CASE("A scaled negative timeval stays negative", "[base][chrono]") {
  const rex::f64 scalar = rex::Clock::GetGuestTimeScalar();
  rex::Clock::SetGuestTimeScalar(2.0);
  rex::i32 sec = -1;
  rex::i32 usec = -1;
  rex::Clock::ScaleGuestDurationTimeval(&sec, &usec);
  CHECK(sec == -3);
  CHECK(usec == 999'998);
  rex::Clock::SetGuestTimeScalar(scalar);
}

REX_TEST_CASE("A scaled timeval carries microseconds into seconds at 1 second", "[base][chrono]") {
  const rex::f64 scalar = rex::Clock::GetGuestTimeScalar();
  rex::Clock::SetGuestTimeScalar(2.0);
  rex::i32 sec = 0;
  rex::i32 usec = 600'000;
  rex::Clock::ScaleGuestDurationTimeval(&sec, &usec);
  CHECK(sec == 1);
  CHECK(usec == 200'000);
  sec = -1;
  usec = -600'000;
  rex::Clock::ScaleGuestDurationTimeval(&sec, &usec);
  CHECK(sec == -4);
  CHECK(usec == 800'000);
  rex::Clock::SetGuestTimeScalar(scalar);
}

REX_TEST_CASE("A scaled timeval keeps the fraction of its seconds", "[base][chrono]") {
  const rex::f64 scalar = rex::Clock::GetGuestTimeScalar();
  rex::Clock::SetGuestTimeScalar(0.5);
  rex::i32 sec = 1;
  rex::i32 usec = 0;
  rex::Clock::ScaleGuestDurationTimeval(&sec, &usec);
  CHECK(sec == 0);
  CHECK(usec == 500'000);
  sec = 0;
  usec = 1;
  rex::Clock::ScaleGuestDurationTimeval(&sec, &usec);
  CHECK(sec == 0);
  CHECK(usec == 0);
  rex::Clock::SetGuestTimeScalar(scalar);
}
