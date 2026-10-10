/**
 * @file        tests/unit/core/chrono_test.cpp
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

#include <rex/chrono/chrono.h>
#include <rex/chrono/clock.h>

using rex::WinSystemClock;

// All values are 100 nanosecond intervals since 1601-01-01 00:00:00 UTC.
static constexpr u64 kFtNtEpoch = 0;                    /**< 1601-01-01 */
static constexpr u64 kFtUnixEpoch = 116444736000000000; /**< 1970-01-01 */
static constexpr u64 kFtY2k = 125911584000000000;       /**< 2000-01-01 */
static constexpr u64 kFtLeapDay = 125962560000000000;   /**< 2000-02-29 */
static constexpr u64 kFtSubDay = 132538032123450000;    /**< 2020-12-30 12:00:12.345 */
static constexpr u64 kFt2021 = 132539328000000000;      /**< 2021-01-01 */

REX_TEST_CASE("unix_epoch_delta computes correct 1601-to-1970 offset", "[chrono]") {
  // 369 years from 1601 to 1970, with 89 leap days
  constexpr i64 expected_seconds = (369LL * 365 + 89) * 86400LL;

  CHECK(WinSystemClock::unix_epoch_delta().count() == -expected_seconds);
  CHECK(WinSystemClock::unix_epoch_delta().count() < 0);
}

REX_TEST_CASE("from_file_time and to_file_time are exact inverses", "[chrono]") {
  SECTION("zero (NT epoch)") {
    auto tp = WinSystemClock::from_file_time(kFtNtEpoch);
    CHECK(WinSystemClock::to_file_time(tp) == kFtNtEpoch);
  }
  SECTION("Unix epoch") {
    auto tp = WinSystemClock::from_file_time(kFtUnixEpoch);
    CHECK(WinSystemClock::to_file_time(tp) == kFtUnixEpoch);
  }
  SECTION("large value") {
    // Near year 9999.
    constexpr u64 large = 2650467743990000000ULL;
    auto tp = WinSystemClock::from_file_time(large);
    CHECK(WinSystemClock::to_file_time(tp) == large);
  }
}

REX_TEST_CASE("to_file_time then from_file_time round-trips", "[chrono]") {
  auto tp = WinSystemClock::from_file_time(kFtSubDay);
  auto ft = WinSystemClock::to_file_time(tp);
  auto tp2 = WinSystemClock::from_file_time(ft);
  CHECK(tp == tp2);
}

REX_TEST_CASE("to_sys converts known FILETIMEs to correct system_clock values", "[chrono]") {
  using namespace std::chrono;

  SECTION("Unix epoch FILETIME -> system_clock epoch") {
    auto tp = WinSystemClock::to_sys(WinSystemClock::from_file_time(kFtUnixEpoch));
    auto since_epoch = tp.time_since_epoch();
    CHECK(duration_cast<seconds>(since_epoch).count() == 0);
  }
  SECTION("Y2K FILETIME -> 30 years after Unix epoch") {
    auto tp = WinSystemClock::to_sys(WinSystemClock::from_file_time(kFtY2k));
    auto since_epoch = tp.time_since_epoch();
    // 2000-01-01 is 10957 days after 1970-01-01 (including leap days)
    CHECK(duration_cast<seconds>(since_epoch).count() == 10957LL * 86400);
  }
  SECTION("2021 FILETIME -> known seconds since Unix epoch") {
    auto tp = WinSystemClock::to_sys(WinSystemClock::from_file_time(kFt2021));
    auto since_epoch = tp.time_since_epoch();
    // 2021-01-01 is 18628 days after 1970-01-01
    CHECK(duration_cast<seconds>(since_epoch).count() == 18628LL * 86400);
  }
}

REX_TEST_CASE("from_sys then to_sys round-trips for whole-second values", "[chrono]") {
  for (u64 ft : {kFtNtEpoch, kFtUnixEpoch, kFtY2k, kFt2021}) {
    auto nt_tp = WinSystemClock::from_file_time(ft);
    auto sys_tp = WinSystemClock::to_sys(nt_tp);
    auto nt_tp2 = WinSystemClock::from_sys(sys_tp);
    auto ft2 = WinSystemClock::to_file_time(nt_tp2);
    CHECK(ft2 == ft);
  }
}

/** Fields extracted by RtlTimeToTimeFields_entry. */
struct TimeFields {
  int year;
  unsigned month;
  unsigned day;
  unsigned weekday; /**< 0 is Sunday through 6 is Saturday */
  int hours;
  int minutes;
  int seconds;
  int milliseconds;
};

/**
 * Decomposes a FILETIME the way RtlTimeToTimeFields_entry does.
 *
 * @param filetime The FILETIME in 100 nanosecond intervals since 1601-01-01.
 * @return The calendar fields.
 */
static TimeFields decompose(u64 filetime) {
  auto tp = WinSystemClock::to_sys(WinSystemClock::from_file_time(filetime));
  auto dp = std::chrono::floor<std::chrono::days>(tp);
  auto ymd = std::chrono::year_month_day{dp};
  auto wd = std::chrono::weekday{dp};
  auto time = std::chrono::hh_mm_ss{std::chrono::floor<std::chrono::milliseconds>(tp - dp)};
  return {
      static_cast<int>(ymd.year()),
      static_cast<unsigned>(ymd.month()),
      static_cast<unsigned>(ymd.day()),
      wd.c_encoding(),
      static_cast<int>(time.hours().count()),
      static_cast<int>(time.minutes().count()),
      static_cast<int>(time.seconds().count()),
      static_cast<int>(time.subseconds().count()),
  };
}

REX_TEST_CASE("calendar decomposition: NT epoch (1601-01-01)", "[chrono]") {
  auto tf = decompose(kFtNtEpoch);
  CHECK(tf.year == 1601);
  CHECK(tf.month == 1);
  CHECK(tf.day == 1);
  CHECK(tf.weekday == 1);
  CHECK(tf.hours == 0);
  CHECK(tf.minutes == 0);
  CHECK(tf.seconds == 0);
  CHECK(tf.milliseconds == 0);
}

REX_TEST_CASE("calendar decomposition: Unix epoch (1970-01-01)", "[chrono]") {
  auto tf = decompose(kFtUnixEpoch);
  CHECK(tf.year == 1970);
  CHECK(tf.month == 1);
  CHECK(tf.day == 1);
  CHECK(tf.weekday == 4);
  CHECK(tf.hours == 0);
  CHECK(tf.minutes == 0);
  CHECK(tf.seconds == 0);
  CHECK(tf.milliseconds == 0);
}

REX_TEST_CASE("calendar decomposition: Y2K (2000-01-01)", "[chrono]") {
  auto tf = decompose(kFtY2k);
  CHECK(tf.year == 2000);
  CHECK(tf.month == 1);
  CHECK(tf.day == 1);
  CHECK(tf.weekday == 6);
  CHECK(tf.hours == 0);
  CHECK(tf.minutes == 0);
  CHECK(tf.seconds == 0);
  CHECK(tf.milliseconds == 0);
}

REX_TEST_CASE("calendar decomposition: leap day (2000-02-29)", "[chrono]") {
  auto tf = decompose(kFtLeapDay);
  CHECK(tf.year == 2000);
  CHECK(tf.month == 2);
  CHECK(tf.day == 29);
  CHECK(tf.weekday == 2);
  CHECK(tf.hours == 0);
  CHECK(tf.minutes == 0);
  CHECK(tf.seconds == 0);
  CHECK(tf.milliseconds == 0);
}

REX_TEST_CASE("calendar decomposition: sub-day (2020-12-30 12:00:12.345)", "[chrono]") {
  auto tf = decompose(kFtSubDay);
  CHECK(tf.year == 2020);
  CHECK(tf.month == 12);
  CHECK(tf.day == 30);
  CHECK(tf.weekday == 3);
  CHECK(tf.hours == 12);
  CHECK(tf.minutes == 0);
  CHECK(tf.seconds == 12);
  CHECK(tf.milliseconds == 345);
}

REX_TEST_CASE("calendar decomposition: 2021-01-01", "[chrono]") {
  auto tf = decompose(kFt2021);
  CHECK(tf.year == 2021);
  CHECK(tf.month == 1);
  CHECK(tf.day == 1);
  CHECK(tf.weekday == 5);
  CHECK(tf.hours == 0);
  CHECK(tf.minutes == 0);
  CHECK(tf.seconds == 0);
  CHECK(tf.milliseconds == 0);
}

/**
 * Recomposes calendar fields into a FILETIME the way RtlTimeFieldsToTime_entry does.
 *
 * @param y The year.
 * @param m The month.
 * @param d The day of the month.
 * @param hour The hour.
 * @param min The minute.
 * @param sec The second.
 * @param ms The millisecond.
 * @return The FILETIME, or 0 when the date is invalid.
 */
static u64 recompose(int y, unsigned m, unsigned d, int hour, int min, int sec, int ms) {
  auto ymd =
      std::chrono::year_month_day{std::chrono::year{y}, std::chrono::month{m}, std::chrono::day{d}};
  if (!ymd.ok())
    return 0;
  auto dp = static_cast<std::chrono::sys_days>(ymd);
  std::chrono::sys_time<rex::hundrednanoseconds> time = dp;
  time += std::chrono::hours{hour};
  time += std::chrono::minutes{min};
  time += std::chrono::seconds{sec};
  time += std::chrono::milliseconds{ms};
  return WinSystemClock::to_file_time(WinSystemClock::from_sys(time));
}

REX_TEST_CASE("calendar recomposition: known dates produce correct FILETIMEs", "[chrono]") {
  CHECK(recompose(1601, 1, 1, 0, 0, 0, 0) == kFtNtEpoch);
  CHECK(recompose(1970, 1, 1, 0, 0, 0, 0) == kFtUnixEpoch);
  CHECK(recompose(2000, 1, 1, 0, 0, 0, 0) == kFtY2k);
  CHECK(recompose(2000, 2, 29, 0, 0, 0, 0) == kFtLeapDay);
  CHECK(recompose(2020, 12, 30, 12, 0, 12, 345) == kFtSubDay);
  CHECK(recompose(2021, 1, 1, 0, 0, 0, 0) == kFt2021);
}

REX_TEST_CASE("calendar recomposition: decompose then recompose round-trips", "[chrono]") {
  for (u64 ft : {kFtNtEpoch, kFtUnixEpoch, kFtY2k, kFtLeapDay, kFtSubDay, kFt2021}) {
    auto tf = decompose(ft);
    auto result =
        recompose(tf.year, tf.month, tf.day, tf.hours, tf.minutes, tf.seconds, tf.milliseconds);
    CHECK(result == ft);
  }
}

REX_TEST_CASE("year_month_day::ok rejects invalid dates", "[chrono]") {
  CHECK_FALSE(std::chrono::year_month_day{std::chrono::year{2000}, std::chrono::month{2},
                                          std::chrono::day{30}}
                  .ok());
  CHECK_FALSE(std::chrono::year_month_day{std::chrono::year{2000}, std::chrono::month{13},
                                          std::chrono::day{1}}
                  .ok());
  CHECK_FALSE(std::chrono::year_month_day{std::chrono::year{2000}, std::chrono::month{1},
                                          std::chrono::day{0}}
                  .ok());
  CHECK_FALSE(std::chrono::year_month_day{std::chrono::year{2001}, std::chrono::month{2},
                                          std::chrono::day{29}}
                  .ok());
  CHECK(std::chrono::year_month_day{std::chrono::year{2000}, std::chrono::month{2},
                                    std::chrono::day{29}}
            .ok());
  CHECK_FALSE(std::chrono::year_month_day{std::chrono::year{1900}, std::chrono::month{2},
                                          std::chrono::day{29}}
                  .ok());
  CHECK(std::chrono::year_month_day{std::chrono::year{2000}, std::chrono::month{2},
                                    std::chrono::day{29}}
            .ok());
}

REX_TEST_CASE("recompose returns 0 for invalid dates", "[chrono]") {
  CHECK(recompose(2000, 2, 30, 0, 0, 0, 0) == 0);
  CHECK(recompose(2001, 2, 29, 0, 0, 0, 0) == 0);
  CHECK(recompose(2000, 13, 1, 0, 0, 0, 0) == 0);
  CHECK(recompose(2000, 1, 0, 0, 0, 0, 0) == 0);
}

REX_TEST_CASE("weekday c_encoding returns 0=Sunday through 6=Saturday", "[chrono]") {
  using namespace std::chrono;

  // 2000-01-02 is Sunday (0)
  CHECK(weekday{sys_days{year{2000} / month{1} / day{2}}}.c_encoding() == 0);
  // 1601-01-01 is Monday (1)
  CHECK(weekday{sys_days{year{1601} / month{1} / day{1}}}.c_encoding() == 1);
  // 2000-02-29 is Tuesday (2)
  CHECK(weekday{sys_days{year{2000} / month{2} / day{29}}}.c_encoding() == 2);
  // 2020-12-30 is Wednesday (3)
  CHECK(weekday{sys_days{year{2020} / month{12} / day{30}}}.c_encoding() == 3);
  // 1970-01-01 is Thursday (4)
  CHECK(weekday{sys_days{year{1970} / month{1} / day{1}}}.c_encoding() == 4);
  // 2021-01-01 is Friday (5)
  CHECK(weekday{sys_days{year{2021} / month{1} / day{1}}}.c_encoding() == 5);
  // 2000-01-01 is Saturday (6)
  CHECK(weekday{sys_days{year{2000} / month{1} / day{1}}}.c_encoding() == 6);
}

REX_TEST_CASE("The host tick count advances at the rate of steady_clock", "[chrono]") {
  const auto start = std::chrono::steady_clock::now();
  const rex::u64 ticks_start = rex::Clock::QueryHostTickCount();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  const rex::u64 ticks = rex::Clock::QueryHostTickCount() - ticks_start;
  const auto elapsed = std::chrono::duration<rex::f64>(std::chrono::steady_clock::now() - start);
  const rex::f64 seconds =
      static_cast<rex::f64>(ticks) / static_cast<rex::f64>(rex::Clock::QueryHostTickFrequency());
  CHECK(seconds / elapsed.count() > 0.99);
  CHECK(seconds / elapsed.count() < 1.01);
}

REX_TEST_CASE("A scaled relative file time stays negative", "[chrono]") {
  const rex::f64 scalar = rex::Clock::guest_time_scalar();
  rex::Clock::set_guest_time_scalar(0.5);
  CHECK(rex::Clock::ScaleGuestDurationFileTime(-10'000'000) == -5'000'000);
  rex::Clock::set_guest_time_scalar(2.0);
  CHECK(rex::Clock::ScaleGuestDurationFileTime(-10'000'000) == -20'000'000);
  rex::Clock::set_guest_time_scalar(scalar);
}

REX_TEST_CASE("A scaled negative timeval stays negative", "[chrono]") {
  const rex::f64 scalar = rex::Clock::guest_time_scalar();
  rex::Clock::set_guest_time_scalar(2.0);
  rex::i32 sec = -1;
  rex::i32 usec = -1;
  rex::Clock::ScaleGuestDurationTimeval(&sec, &usec);
  CHECK(sec == -3);
  CHECK(usec == 999'998);
  rex::Clock::set_guest_time_scalar(scalar);
}

REX_TEST_CASE("A scaled timeval carries microseconds into seconds at 1 second", "[chrono]") {
  const rex::f64 scalar = rex::Clock::guest_time_scalar();
  rex::Clock::set_guest_time_scalar(2.0);
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
  rex::Clock::set_guest_time_scalar(scalar);
}

REX_TEST_CASE("A scaled timeval keeps the fraction of its seconds", "[chrono]") {
  const rex::f64 scalar = rex::Clock::guest_time_scalar();
  rex::Clock::set_guest_time_scalar(0.5);
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
  rex::Clock::set_guest_time_scalar(scalar);
}
