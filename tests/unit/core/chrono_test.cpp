/**
 * @file        tests/unit/core/chrono_test.cpp
 * @brief       Unit tests for rex::chrono types (NtSystemClock, WinSystemClock)
 *              and the date:: calendar API surface used in xboxkrnl_rtl.cpp.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <chrono>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/chrono/chrono.h>

using rex::chrono::WinSystemClock;

// =============================================================================
// Known FILETIME constants
// =============================================================================

// All values are 100-nanosecond intervals since 1601-01-01 00:00:00 UTC.
static constexpr uint64_t kFtNtEpoch = 0;                     // 1601-01-01
static constexpr uint64_t kFtUnixEpoch = 116444736000000000;  // 1970-01-01
static constexpr uint64_t kFtY2k = 125911584000000000;        // 2000-01-01
static constexpr uint64_t kFtLeapDay = 125962560000000000;    // 2000-02-29
static constexpr uint64_t kFtSubDay = 132538032123450000;     // 2020-12-30 12:00:12.345
static constexpr uint64_t kFt2021 = 132539328000000000;       // 2021-01-01

// =============================================================================
// Section 1: Epoch Constant Validation
// =============================================================================

TEST_CASE("unix_epoch_delta computes correct 1601-to-1970 offset", "[chrono]") {
  // 369 years from 1601 to 1970, with 89 leap days
  constexpr int64_t expected_seconds = (369LL * 365 + 89) * 86400LL;

  // unix_epoch_delta() computes dynamically from date types
  // The delta should be negative (1601 is before 1970)
  CHECK(WinSystemClock::unix_epoch_delta().count() == -expected_seconds);
  CHECK(WinSystemClock::unix_epoch_delta().count() < 0);
}

// =============================================================================
// Section 2: FILETIME Round-Trip
// =============================================================================

TEST_CASE("from_file_time and to_file_time are exact inverses", "[chrono]") {
  SECTION("zero (NT epoch)") {
    auto tp = WinSystemClock::from_file_time(kFtNtEpoch);
    CHECK(WinSystemClock::to_file_time(tp) == kFtNtEpoch);
  }
  SECTION("Unix epoch") {
    auto tp = WinSystemClock::from_file_time(kFtUnixEpoch);
    CHECK(WinSystemClock::to_file_time(tp) == kFtUnixEpoch);
  }
  SECTION("large value") {
    constexpr uint64_t large = 2650467743990000000ULL;  // ~year 9999
    auto tp = WinSystemClock::from_file_time(large);
    CHECK(WinSystemClock::to_file_time(tp) == large);
  }
}

TEST_CASE("to_file_time then from_file_time round-trips", "[chrono]") {
  auto tp = WinSystemClock::from_file_time(kFtSubDay);
  auto ft = WinSystemClock::to_file_time(tp);
  auto tp2 = WinSystemClock::from_file_time(ft);
  CHECK(tp == tp2);
}

// =============================================================================
// Section 3: to_sys / from_sys with Known Values
// =============================================================================

TEST_CASE("to_sys converts known FILETIMEs to correct system_clock values", "[chrono]") {
  using namespace std::chrono;

  SECTION("Unix epoch FILETIME -> system_clock epoch") {
    auto tp = WinSystemClock::to_sys(WinSystemClock::from_file_time(kFtUnixEpoch));
    // system_clock epoch is 1970-01-01, so time_since_epoch should be zero
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

TEST_CASE("from_sys then to_sys round-trips for whole-second values", "[chrono]") {
  // Use values aligned to system_clock precision (whole seconds)
  for (uint64_t ft : {kFtNtEpoch, kFtUnixEpoch, kFtY2k, kFt2021}) {
    auto nt_tp = WinSystemClock::from_file_time(ft);
    auto sys_tp = WinSystemClock::to_sys(nt_tp);
    auto nt_tp2 = WinSystemClock::from_sys(sys_tp);
    auto ft2 = WinSystemClock::to_file_time(nt_tp2);
    CHECK(ft2 == ft);
  }
}

// =============================================================================
// Section 4: Calendar Decomposition (mirrors RtlTimeToTimeFields)
// =============================================================================

// Helper struct matching the fields extracted in RtlTimeToTimeFields_entry
struct TimeFields {
  int year;
  unsigned month;
  unsigned day;
  unsigned weekday;  // c_encoding: 0=Sun..6=Sat
  int hours;
  int minutes;
  int seconds;
  int milliseconds;
};

// Decompose a FILETIME exactly as RtlTimeToTimeFields_entry does
static TimeFields decompose(uint64_t filetime) {
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

TEST_CASE("calendar decomposition: NT epoch (1601-01-01)", "[chrono]") {
  auto tf = decompose(kFtNtEpoch);
  CHECK(tf.year == 1601);
  CHECK(tf.month == 1);
  CHECK(tf.day == 1);
  CHECK(tf.weekday == 1);  // Monday
  CHECK(tf.hours == 0);
  CHECK(tf.minutes == 0);
  CHECK(tf.seconds == 0);
  CHECK(tf.milliseconds == 0);
}

TEST_CASE("calendar decomposition: Unix epoch (1970-01-01)", "[chrono]") {
  auto tf = decompose(kFtUnixEpoch);
  CHECK(tf.year == 1970);
  CHECK(tf.month == 1);
  CHECK(tf.day == 1);
  CHECK(tf.weekday == 4);  // Thursday
  CHECK(tf.hours == 0);
  CHECK(tf.minutes == 0);
  CHECK(tf.seconds == 0);
  CHECK(tf.milliseconds == 0);
}

TEST_CASE("calendar decomposition: Y2K (2000-01-01)", "[chrono]") {
  auto tf = decompose(kFtY2k);
  CHECK(tf.year == 2000);
  CHECK(tf.month == 1);
  CHECK(tf.day == 1);
  CHECK(tf.weekday == 6);  // Saturday
  CHECK(tf.hours == 0);
  CHECK(tf.minutes == 0);
  CHECK(tf.seconds == 0);
  CHECK(tf.milliseconds == 0);
}

TEST_CASE("calendar decomposition: leap day (2000-02-29)", "[chrono]") {
  auto tf = decompose(kFtLeapDay);
  CHECK(tf.year == 2000);
  CHECK(tf.month == 2);
  CHECK(tf.day == 29);
  CHECK(tf.weekday == 2);  // Tuesday
  CHECK(tf.hours == 0);
  CHECK(tf.minutes == 0);
  CHECK(tf.seconds == 0);
  CHECK(tf.milliseconds == 0);
}

TEST_CASE("calendar decomposition: sub-day (2020-12-30 12:00:12.345)", "[chrono]") {
  auto tf = decompose(kFtSubDay);
  CHECK(tf.year == 2020);
  CHECK(tf.month == 12);
  CHECK(tf.day == 30);
  CHECK(tf.weekday == 3);  // Wednesday
  CHECK(tf.hours == 12);
  CHECK(tf.minutes == 0);
  CHECK(tf.seconds == 12);
  CHECK(tf.milliseconds == 345);
}

TEST_CASE("calendar decomposition: 2021-01-01", "[chrono]") {
  auto tf = decompose(kFt2021);
  CHECK(tf.year == 2021);
  CHECK(tf.month == 1);
  CHECK(tf.day == 1);
  CHECK(tf.weekday == 5);  // Friday
  CHECK(tf.hours == 0);
  CHECK(tf.minutes == 0);
  CHECK(tf.seconds == 0);
  CHECK(tf.milliseconds == 0);
}

// =============================================================================
// Section 5: Calendar Recomposition (mirrors RtlTimeFieldsToTime)
// =============================================================================

// Recompose fields to FILETIME exactly as RtlTimeFieldsToTime_entry does
static uint64_t recompose(int y, unsigned m, unsigned d, int hour, int min, int sec, int ms) {
  auto ymd =
      std::chrono::year_month_day{std::chrono::year{y}, std::chrono::month{m}, std::chrono::day{d}};
  if (!ymd.ok())
    return 0;
  auto dp = static_cast<std::chrono::sys_days>(ymd);
  std::chrono::system_clock::time_point time = dp;
  time += std::chrono::hours{hour};
  time += std::chrono::minutes{min};
  time += std::chrono::seconds{sec};
  time += std::chrono::milliseconds{ms};
  return WinSystemClock::to_file_time(WinSystemClock::from_sys(time));
}

TEST_CASE("calendar recomposition: known dates produce correct FILETIMEs", "[chrono]") {
  CHECK(recompose(1601, 1, 1, 0, 0, 0, 0) == kFtNtEpoch);
  CHECK(recompose(1970, 1, 1, 0, 0, 0, 0) == kFtUnixEpoch);
  CHECK(recompose(2000, 1, 1, 0, 0, 0, 0) == kFtY2k);
  CHECK(recompose(2000, 2, 29, 0, 0, 0, 0) == kFtLeapDay);
  CHECK(recompose(2020, 12, 30, 12, 0, 12, 345) == kFtSubDay);
  CHECK(recompose(2021, 1, 1, 0, 0, 0, 0) == kFt2021);
}

TEST_CASE("calendar recomposition: decompose then recompose round-trips", "[chrono]") {
  for (uint64_t ft : {kFtNtEpoch, kFtUnixEpoch, kFtY2k, kFtLeapDay, kFtSubDay, kFt2021}) {
    auto tf = decompose(ft);
    auto result =
        recompose(tf.year, tf.month, tf.day, tf.hours, tf.minutes, tf.seconds, tf.milliseconds);
    CHECK(result == ft);
  }
}

TEST_CASE("year_month_day::ok rejects invalid dates", "[chrono]") {
  // Feb 30 - never valid
  CHECK_FALSE(std::chrono::year_month_day{std::chrono::year{2000}, std::chrono::month{2},
                                          std::chrono::day{30}}
                  .ok());
  // Month 13 - invalid month
  CHECK_FALSE(std::chrono::year_month_day{std::chrono::year{2000}, std::chrono::month{13},
                                          std::chrono::day{1}}
                  .ok());
  // Day 0 - invalid day
  CHECK_FALSE(std::chrono::year_month_day{std::chrono::year{2000}, std::chrono::month{1},
                                          std::chrono::day{0}}
                  .ok());
  // Feb 29 in non-leap year
  CHECK_FALSE(std::chrono::year_month_day{std::chrono::year{2001}, std::chrono::month{2},
                                          std::chrono::day{29}}
                  .ok());
  // Feb 29 in leap year - valid
  CHECK(std::chrono::year_month_day{std::chrono::year{2000}, std::chrono::month{2},
                                    std::chrono::day{29}}
            .ok());
  // Century non-leap: 1900 is not a leap year
  CHECK_FALSE(std::chrono::year_month_day{std::chrono::year{1900}, std::chrono::month{2},
                                          std::chrono::day{29}}
                  .ok());
  // 400-year leap: 2000 is a leap year (already checked above, but explicit)
  CHECK(std::chrono::year_month_day{std::chrono::year{2000}, std::chrono::month{2},
                                    std::chrono::day{29}}
            .ok());
}

TEST_CASE("recompose returns 0 for invalid dates", "[chrono]") {
  CHECK(recompose(2000, 2, 30, 0, 0, 0, 0) == 0);
  CHECK(recompose(2001, 2, 29, 0, 0, 0, 0) == 0);
  CHECK(recompose(2000, 13, 1, 0, 0, 0, 0) == 0);
  CHECK(recompose(2000, 1, 0, 0, 0, 0, 0) == 0);
}

// =============================================================================
// Section 6: Weekday c_encoding Edge Cases
// =============================================================================

TEST_CASE("weekday c_encoding returns 0=Sunday through 6=Saturday", "[chrono]") {
  using namespace std::chrono;

  // Verify the full range of c_encoding values using known days
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
