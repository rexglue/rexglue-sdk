/**
 * @file        chrono.h
 * @brief       Host and guest clocks over NT time
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/chrono.h and xenia/base/chrono_steady_cast.h
 *              (Ben Vanik, 2022).
 *
 * @remarks     Derived from Xenia's xenia/base/clock.h (Ben Vanik, 2022).
 */

#pragma once

#include <atomic>
#include <chrono>
#include <utility>

#include <rex/platform.h>
#include <rex/types.h>

namespace rex {

/**
 * Host clock queries and the scaled guest clock.
 */
class REX_API Clock {
 public:
  /**
   * Reports whether guest time follows host time unscaled.
   *
   * @return True when the clock.no_scaling cvar is set.
   */
  static bool IsNoScaling();

  static u64 GetHostTickFrequency() noexcept;

  static u64 GetHostTickCount() noexcept;

  /**
   * @return The host system time as a FILETIME, in 100ns units since Jan 1 1601.
   */
  static u64 QueryHostSystemTime() noexcept;

  /** Returns the host uptime in milliseconds. */
  static u64 QueryHostUptimeMillis();

  /** 1 is normal speed. */
  static f64 GetGuestTimeScalar();

  /**
   * Sets the guest time scalar. 2 runs at double speed and 0.5 at half speed. Ignored when
   * no_scaling is true.
   */
  static void SetGuestTimeScalar(f64 scalar);

  /**
   * @return The host to guest tick ratio with time scaling applied, as a reduced numerator and
   *         denominator pair.
   */
  static std::pair<u64, u64> GetGuestTickRatio();

  static u64 GetGuestTickFrequency();

  /**
   * Sets the guest tick frequency and recomputes the tick ratio.
   *
   * @param frequency Guest ticks per second.
   */
  static void SetGuestTickFrequency(u64 frequency);

  /**
   * Returns the FILETIME base of the guest system time. It defaults to the host system time at
   * startup.
   */
  static u64 GetGuestSystemTimeBase();

  /**
   * Sets the FILETIME base the guest system time counts from.
   *
   * @param time_base The new base as a FILETIME.
   */
  static void SetGuestSystemTimeBase(u64 time_base);

  /** Advances and reads the guest tick count, with frequency adjustment and scaling applied. */
  static u64 QueryGuestTickCount();

  /**
   * @return The guest system time as a scaled FILETIME. It equals host system time when
   *         no_scaling is true.
   */
  static u64 QueryGuestSystemTime();

  /** Returns the title uptime in milliseconds. The time is scaled and clamped to the u32 range. */
  static u32 GetGuestUptimeMillis();

  /**
   * Sets the guest system time by moving the time base. Ignored when no_scaling is true.
   *
   * @param system_time The new guest system time as a FILETIME.
   */
  static void SetGuestSystemTime(u64 system_time);

  /**
   * Scales a guest duration in milliseconds by the time scalar.
   *
   * @return The scaled duration, clamped to the u32 range.
   */
  static u32 ScaleGuestDurationMillis(u32 guest_ms);

  /**
   * Scales a guest FILETIME duration by the time scalar. A positive value is an absolute time,
   * scaled by its offset from the current guest system time. A negative value is a relative
   * duration.
   *
   * @param guest_file_time The guest time in 100ns units.
   * @return The scaled time in 100ns units.
   */
  static i64 ScaleGuestDurationFileTime(i64 guest_file_time);

  /**
   * Scales a guest timeval duration in place by the time scalar.
   *
   * @param tv_sec The seconds field, read and written.
   * @param tv_usec The microseconds field, read and written.
   */
  static void ScaleGuestDurationTimeval(i32* tv_sec, i32* tv_usec);
};

/** Ratio of 1 FILETIME tick, 100ns, to 1 second. */
using HundredNano = std::ratio<1, 10000000>;

/** Duration in 100ns FILETIME ticks. */
using HundredNanoseconds = std::chrono::duration<i64, HundredNano>;

namespace detail {

enum class Domain : u8 {
  kHost, /**< Unscaled host time. */
  kGuest /**< Guest time, with the Clock time scalar and time base applied. */
};

/** std::chrono clock over NT FILETIME, 100ns ticks since Jan 1 1601. */
template <Domain Domain>
struct NtSystemClock {
  using rep = i64;
  using period = HundredNano;
  using duration = HundredNanoseconds;
  using time_point = std::chrono::time_point<NtSystemClock<Domain>>;

 public:
  /**
   * Returns the offset from the system clock epoch to the FILETIME epoch. The epochs are Jan 1 1970
   * and Jan 1 1601, in seconds.
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

  static constexpr u64 ToFileTime(time_point const& tp) noexcept {
    return static_cast<u64>(tp.time_since_epoch().count());
  }

  static constexpr time_point FromFileTime(u64 const& tp) noexcept {
    return time_point{duration{tp}};
  }

  /**
   * Converts a host time point to system time in 100ns ticks. Convert XSystemClock time with
   * clock_cast to WinSystemClock first.
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
   */
  template <typename SysDuration>
  static constexpr time_point from_sys(const std::chrono::sys_time<SysDuration>& tp)
    requires(Domain == Domain::kHost)
  {
    auto ctp = std::chrono::time_point_cast<duration>(tp);
    return time_point{ctp.time_since_epoch() - GetUnixEpochDelta()};
  }

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

/**
 * Converts a FAT date and time to Windows FILETIME ticks.
 *
 * @param date FAT date, 7 bits of years since 1980, 4 bits of month, 5 bits of day.
 * @param time FAT time, 5 bits of hours, 6 bits of minutes, 5 bits of 2 second units.
 * @return 100 nanosecond intervals since 1601-01-01 UTC. An out of range field carries into the
 * next field.
 */
inline u64 DecodeFATTimestamp(u32 date, u32 time) {
  using namespace std::chrono;
  const year_month first = year{1980 + static_cast<i32>((date >> 9) & 0x7F)} / January;
  const year_month month = first + months{static_cast<i32>((date >> 5) & 0x0F) - 1};
  const sys_days day = sys_days{month / 1} + days{static_cast<i32>(date & 0x1F) - 1};
  const sys_seconds stamp = day + hours{static_cast<i32>((time >> 11) & 0x1F)} +
                            minutes{static_cast<i32>((time >> 5) & 0x3F)} +
                            seconds{static_cast<i32>((time & 0x1F) * 2)};
  const i64 file_time_seconds =
      stamp.time_since_epoch().count() -
      detail::NtSystemClock<detail::Domain::kHost>::GetUnixEpochDelta().count();
  return static_cast<u64>(file_time_seconds * HundredNano::den);
}

}  // namespace rex

// NOLINTNEXTLINE(bugprone-std-namespace-modification): permitted program defined clock
namespace std::chrono {

#ifdef _LIBCPP_VERSION
/**
 * Replaces the C++20 clock conversion trait, which libc++ does not provide.
 */
template <class, class>
struct clock_time_conversion {};

/** Replaces C++20 clock_cast, which libc++ does not provide. */
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

  /** Converts a guest time point to a host time point. */
  template <typename Duration>
  WClock::time_point operator()(const std::chrono::time_point<XClock, Duration>& t) const {
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto w_now = WClock::now();
    auto x_now = XClock::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    auto delta = (t - x_now);
    if (!rex::Clock::IsNoScaling()) {
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

  /** Converts a host time point to a guest time point. */
  template <typename Duration>
  XClock::time_point operator()(const std::chrono::time_point<WClock, Duration>& t) const {
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto w_now = WClock::now();
    auto x_now = XClock::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    rex::HundredNanoseconds delta = (t - w_now);
    if (!rex::Clock::IsNoScaling()) {
      delta = std::chrono::floor<WClock::duration>(delta / rex::Clock::GetGuestTimeScalar());
    }
    return x_now + delta;
  }
};

}  // namespace std::chrono

// NOLINTNEXTLINE(bugprone-std-namespace-modification): permitted program defined clock
namespace std::chrono {

/**
 * Converts steady_clock time to WinSystemClock time with a small error. Host time only. Convert
 * XSystemClock time to WinSystemClock first.
 */
template <>
struct clock_time_conversion<::rex::WinSystemClock, std::chrono::steady_clock> {
  /** Destination clock. */
  using WinSystemClock = ::rex::WinSystemClock;
  /** Source clock. */
  using SteadyClock = std::chrono::steady_clock;

  /** Converts a steady_clock time point. */
  template <typename Duration>
  WinSystemClock::time_point operator()(
      const std::chrono::time_point<SteadyClock, Duration>& t) const {
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto steady_now = SteadyClock::now();
    auto nt_now = WinSystemClock::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    auto delta = std::chrono::floor<WinSystemClock::duration>(t - steady_now);
    return nt_now + delta;
  }
};

/** Converts WinSystemClock time to steady_clock time, for NT waits. */
template <>
struct clock_time_conversion<std::chrono::steady_clock, ::rex::WinSystemClock> {
  /** Source clock. */
  using WinSystemClock = ::rex::WinSystemClock;
  /** Destination clock. */
  using SteadyClock = std::chrono::steady_clock;

  /** Converts a WinSystemClock time point. */
  template <typename Duration>
  SteadyClock::time_point operator()(
      const std::chrono::time_point<WinSystemClock, Duration>& t) const {
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto steady_now = SteadyClock::now();
    auto nt_now = WinSystemClock::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    auto delta = t - nt_now;
    return steady_now + delta;
  }
};

}  // namespace std::chrono
