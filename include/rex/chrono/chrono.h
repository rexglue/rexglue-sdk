/**
 * @file        chrono/chrono.h
 * @brief       NT FILETIME clocks for host and guest time with std::chrono conversions
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
using hundrednano = std::ratio<1, 10000000>;

/** Duration in 100ns FILETIME ticks. */
using hundrednanoseconds = std::chrono::duration<i64, hundrednano>;

namespace detail {

/** Time domain an NtSystemClock reads. */
enum class Domain {
  Host, /**< Unscaled host time. */
  Guest /**< guest time, with the Clock time scalar and time base applied. */
};

/**
 * std::chrono clock over NT FILETIME, 100ns ticks since Jan 1 1601.
 *
 * @tparam domain_ The time domain the clock reads.
 */
template <Domain domain_>
struct NtSystemClock {
  using rep = i64;                                                    /**< Tick count type. */
  using period = hundrednano;                                         /**< Tick period. */
  using duration = hundrednanoseconds;                                /**< Duration type. */
  using time_point = std::chrono::time_point<NtSystemClock<domain_>>; /**< Time point type. */

 public:
  /**
   * @return The offset from the std::chrono::system_clock epoch, Jan 1 1970, to the FILETIME
   *         epoch, Jan 1 1601, in seconds.
   */
  static constexpr std::chrono::seconds unix_epoch_delta() {
    using std::chrono::steady_clock;
    auto filetime_epoch = std::chrono::year{1601} / std::chrono::month{1} / std::chrono::day{1};
    auto system_clock_epoch = std::chrono::year{1970} / std::chrono::month{1} / std::chrono::day{1};
    auto fp =
        static_cast<std::chrono::sys_seconds>(static_cast<std::chrono::sys_days>(filetime_epoch));
    auto sp = static_cast<std::chrono::sys_seconds>(
        static_cast<std::chrono::sys_days>(system_clock_epoch));
    return fp.time_since_epoch() - sp.time_since_epoch();
  }

 public:
  /**
   * @param tp The time point to convert.
   * @return The time point as a FILETIME.
   */
  static constexpr u64 to_file_time(time_point const& tp) noexcept {
    return static_cast<u64>(tp.time_since_epoch().count());
  }

  /**
   * @param tp The FILETIME to convert.
   * @return The FILETIME as a time point.
   */
  static constexpr time_point from_file_time(u64 const& tp) noexcept {
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
    requires(domain_ == Domain::Host)
  {
    return std::chrono::sys_time<duration>{tp.time_since_epoch() + unix_epoch_delta()};
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
    requires(domain_ == Domain::Host)
  {
    auto ctp = std::chrono::time_point_cast<duration>(tp);
    return time_point{ctp.time_since_epoch() - unix_epoch_delta()};
  }

  /**
   * @return The current time from Clock::QueryHostSystemTime or Clock::QueryGuestSystemTime.
   */
  [[nodiscard]] static time_point now() noexcept {
    if constexpr (domain_ == Domain::Host) {
      return from_file_time(Clock::QueryHostSystemTime());
    } else if constexpr (domain_ == Domain::Guest) {
      return from_file_time(Clock::QueryGuestSystemTime());
    }
  }
};

}  // namespace detail

/** Unscaled host FILETIME clock. It converts to and from std::chrono::sys_time. */
using WinSystemClock = detail::NtSystemClock<detail::Domain::Host>;

/** Scaled guest FILETIME clock. Convert it with clock_cast to WinSystemClock. */
using XSystemClock = detail::NtSystemClock<detail::Domain::Guest>;

}  // namespace rex

namespace std::chrono {

#ifdef __APPLE__
/**
 * Stand-in for the C++20 clock conversion trait, which Apple libc++ does not provide.
 */
template <class, class>
struct clock_time_conversion {};

/**
 * Stand-in for C++20 clock_cast, which Apple libc++ does not provide.
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
  using WClock_ = ::rex::WinSystemClock;
  /** Source clock. */
  using XClock_ = ::rex::XSystemClock;

  /**
   * @tparam Duration The duration of the source time point.
   * @param t The guest time point.
   * @return The same instant in host time.
   */
  template <typename Duration>
  typename WClock_::time_point operator()(
      const std::chrono::time_point<XClock_, Duration>& t) const {
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto w_now = WClock_::now();
    auto x_now = XClock_::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    auto delta = (t - x_now);
    if (!rex::Clock::no_scaling()) {
      delta = std::chrono::floor<WClock_::duration>(delta * rex::Clock::guest_time_scalar());
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
  using WClock_ = ::rex::WinSystemClock;
  /** Destination clock. */
  using XClock_ = ::rex::XSystemClock;

  /**
   * @tparam Duration The duration of the source time point.
   * @param t The host time point.
   * @return The same instant in guest time.
   */
  template <typename Duration>
  typename XClock_::time_point operator()(
      const std::chrono::time_point<WClock_, Duration>& t) const {
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto w_now = WClock_::now();
    auto x_now = XClock_::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    rex::hundrednanoseconds delta = (t - w_now);
    if (!rex::Clock::no_scaling()) {
      delta = std::chrono::floor<WClock_::duration>(delta / rex::Clock::guest_time_scalar());
    }
    return x_now + delta;
  }
};

}  // namespace std::chrono
