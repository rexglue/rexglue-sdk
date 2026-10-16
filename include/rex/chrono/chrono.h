/**
 * @file        chrono/chrono.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/chrono.h (Ben Vanik, 2022).
 */

#pragma once

#include <atomic>
#include <chrono>

#include <rex/chrono/clock.h>
#include <rex/types.h>

namespace rex {

/** Ratio of 1 FILETIME tick, 100ns, to 1 second. */
using HundredNano = std::ratio<1, 10000000>;

/** Duration in 100ns FILETIME ticks. */
using HundredNanoseconds = std::chrono::duration<i64, HundredNano>;

namespace detail {

/** Time domain an NtSystemClock reads. */
enum class Domain : u8 {
  kHost, /**< Unscaled host time. */
  kGuest /**< Guest time, with the Clock time scalar and time base applied. */
};

/**
 * std::chrono clock over NT FILETIME, 100ns ticks since Jan 1 1601.
 *
 * @tparam Domain The time domain the clock reads.
 */
template <Domain Domain>
struct NtSystemClock {
  using rep = i64;                                                   /**< Tick count type. */
  using period = HundredNano;                                        /**< Tick period. */
  using duration = HundredNanoseconds;                               /**< Duration type. */
  using time_point = std::chrono::time_point<NtSystemClock<Domain>>; /**< Time point type. */

 public:
  /**
   * @return The offset from the std::chrono::system_clock epoch, Jan 1 1970, to the FILETIME
   *         epoch, Jan 1 1601, in seconds.
   */
  static constexpr std::chrono::seconds GetUnixEpochDelta() {
    using std::chrono::steady_clock;
    auto filetime_epoch = std::chrono::year{1601} / std::chrono::month{1} / std::chrono::day{1};
    auto system_clock_epoch = std::chrono::year{1970} / std::chrono::month{1} / std::chrono::day{1};
    auto fp =
        static_cast<std::chrono::sys_seconds>(static_cast<std::chrono::sys_days>(filetime_epoch));
    auto sp = static_cast<std::chrono::sys_seconds>(
        static_cast<std::chrono::sys_days>(system_clock_epoch));
    return fp.time_since_epoch() - sp.time_since_epoch();
  }

  /**
   * @param tp The time point to convert.
   * @return The time point as a FILETIME.
   */
  static constexpr u64 ToFileTime(time_point const& tp) noexcept {
    return static_cast<u64>(tp.time_since_epoch().count());
  }

  /**
   * @param tp The FILETIME to convert.
   * @return The FILETIME as a time point.
   */
  static constexpr time_point FromFileTime(u64 const& tp) noexcept {
    return time_point{duration{tp}};
  }

  /**
   * Converts a host time point to system time in 100ns ticks, which holds every FILETIME. Convert
   * XSystemClock time with clock_cast to WinSystemClock first.
   *
   * @param tp The host time point.
   * @return The same instant as std::chrono::sys_time.
   */
  static constexpr std::chrono::sys_time<duration> to_sys(const time_point& tp)
    requires(Domain == Domain::kHost)
  {
    return std::chrono::sys_time<duration>{tp.time_since_epoch() + GetUnixEpochDelta()};
  }

  /**
   * Converts system time to host time, truncated to 100ns ticks.
   *
   * @tparam SysDuration The duration of the system time point.
   * @param tp The system time point.
   * @return The same instant on this clock.
   */
  template <typename SysDuration>
  static constexpr time_point from_sys(const std::chrono::sys_time<SysDuration>& tp)
    requires(Domain == Domain::kHost)
  {
    auto ctp = std::chrono::time_point_cast<duration>(tp);
    return time_point{ctp.time_since_epoch() - GetUnixEpochDelta()};
  }

  /**
   * @return The current time from Clock::QueryHostSystemTime or Clock::QueryGuestSystemTime.
   */
  [[nodiscard]] static time_point now() noexcept {
    if constexpr (Domain == Domain::kHost) {
      return FromFileTime(Clock::QueryHostSystemTime());
    } else if constexpr (Domain == Domain::kGuest) {
      return FromFileTime(Clock::QueryGuestSystemTime());
    }
  }
};

}  // namespace detail

/** Unscaled host FILETIME clock. It converts to and from std::chrono::sys_time. */
using WinSystemClock = detail::NtSystemClock<detail::Domain::kHost>;

/** Scaled guest FILETIME clock. Convert it with clock_cast to WinSystemClock. */
using XSystemClock = detail::NtSystemClock<detail::Domain::kGuest>;

}  // namespace rex

// NOLINTNEXTLINE(bugprone-std-namespace-modification): permitted program defined clock
namespace std::chrono {

#ifdef _LIBCPP_VERSION
/**
 * Stand-in for the C++20 clock conversion trait, which libc++ does not provide.
 */
template <class, class>
struct clock_time_conversion {};

/**
 * Stand-in for C++20 clock_cast, which libc++ does not provide.
 *
 * @tparam DestClock The clock to convert to.
 * @tparam SourceClock The clock to convert from.
 * @tparam Duration The duration of the source time point.
 * @param t The time point to convert.
 * @return The same instant on DestClock.
 */
template <class DestClock, class SourceClock, class Duration>
auto clock_cast(const std::chrono::time_point<SourceClock, Duration>& t) {
  return clock_time_conversion<DestClock, SourceClock>{}(t);
}
#endif

/**
 * Converts XSystemClock time to WinSystemClock time. The offset from the current guest time is
 * multiplied by the Clock time scalar unless clock.no_scaling is set.
 */
template <>
struct clock_time_conversion<::rex::WinSystemClock, ::rex::XSystemClock> {
  /** Destination clock. */
  using WClock = ::rex::WinSystemClock;
  /** Source clock. */
  using XClock = ::rex::XSystemClock;

  /**
   * @tparam Duration The duration of the source time point.
   * @param t The guest time point.
   * @return The same instant in host time.
   */
  template <typename Duration>
  WClock::time_point operator()(const std::chrono::time_point<XClock, Duration>& t) const {
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto w_now = WClock::now();
    auto x_now = XClock::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    auto delta = (t - x_now);
    if (!rex::Clock::GetNoScaling()) {
      delta = std::chrono::floor<WClock::duration>(delta * rex::Clock::GetGuestTimeScalar());
    }
    return w_now + delta;
  }
};

/**
 * Converts WinSystemClock time to XSystemClock time. The offset from the current host time is
 * divided by the Clock time scalar unless clock.no_scaling is set.
 */
template <>
struct clock_time_conversion<::rex::XSystemClock, ::rex::WinSystemClock> {
  /** Source clock. */
  using WClock = ::rex::WinSystemClock;
  /** Destination clock. */
  using XClock = ::rex::XSystemClock;

  /**
   * @tparam Duration The duration of the source time point.
   * @param t The host time point.
   * @return The same instant in guest time.
   */
  template <typename Duration>
  XClock::time_point operator()(const std::chrono::time_point<WClock, Duration>& t) const {
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto w_now = WClock::now();
    auto x_now = XClock::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    rex::HundredNanoseconds delta = (t - w_now);
    if (!rex::Clock::GetNoScaling()) {
      delta = std::chrono::floor<WClock::duration>(delta / rex::Clock::GetGuestTimeScalar());
    }
    return x_now + delta;
  }
};

}  // namespace std::chrono
