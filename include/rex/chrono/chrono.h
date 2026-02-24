/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/chrono/clock.h>

namespace rex {
using hundrednano = std::ratio<1, 10000000>;

namespace chrono {

using hundrednanoseconds = std::chrono::duration<int64_t, hundrednano>;

// TODO(JoelLinn) define xstead_clock xsystem_clock etc.

namespace detail {
// Implementation detail: NtSystemClock template for Host/Guest time domains.
// Trick to reduce code duplication and keep all the chrono template magic
// working.
enum class Domain {
  // boring host clock:
  Host,
  // adheres to guest scaling (differrent speed, changing clock drift etc):
  Guest
};

template <Domain domain_>
struct NtSystemClock {
  using rep = int64_t;
  using period = hundrednano;
  using duration = hundrednanoseconds;
  using time_point = std::chrono::time_point<NtSystemClock<domain_>>;
  // This really depends on the context the clock is used in:
  // static constexpr bool is_steady = false;

 public:
  // The delta between std::chrono::system_clock (Jan 1 1970) and NT file
  // time (Jan 1 1601), in seconds. In the spec std::chrono::system_clock's
  // epoch is undefined, but C++20 cements it as Jan 1 1970.
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
  static constexpr uint64_t to_file_time(time_point const& tp) noexcept {
    return static_cast<uint64_t>(tp.time_since_epoch().count());
  }

  static constexpr time_point from_file_time(uint64_t const& tp) noexcept {
    return time_point{duration{tp}};
  }

  // To convert XSystemClock to sys, do clock_cast<WinSystemTime>(tp) first
  // Only available for Host domain (Guest time must be converted via clock_cast)
  static constexpr std::chrono::system_clock::time_point to_sys(const time_point& tp)
    requires(domain_ == Domain::Host)
  {
    using sys_duration = std::chrono::system_clock::duration;
    using sys_time = std::chrono::system_clock::time_point;

    auto dp = tp;
    dp += unix_epoch_delta();
    auto cdp = std::chrono::time_point_cast<sys_duration>(dp);
    return sys_time{cdp.time_since_epoch()};
  }

  static constexpr time_point from_sys(const std::chrono::system_clock::time_point& tp)
    requires(domain_ == Domain::Host)
  {
    auto ctp = std::chrono::time_point_cast<duration>(tp);
    auto dp = time_point{ctp.time_since_epoch()};
    dp -= unix_epoch_delta();
    return dp;
  }

  [[nodiscard]] static time_point now() noexcept {
    if constexpr (domain_ == Domain::Host) {
      // QueryHostSystemTime() returns windows epoch times even on POSIX
      return from_file_time(Clock::QueryHostSystemTime());
    } else if constexpr (domain_ == Domain::Guest) {
      return from_file_time(Clock::QueryGuestSystemTime());
    }
  }
};
}  // namespace detail

// Unscaled system clock which can be used for filetime <-> system_clock
// conversion
using WinSystemClock = detail::NtSystemClock<detail::Domain::Host>;

// Guest system clock, scaled
using XSystemClock = detail::NtSystemClock<detail::Domain::Guest>;

}  // namespace chrono
}  // namespace rex

namespace std::chrono {

template <>
struct clock_time_conversion<::rex::chrono::WinSystemClock, ::rex::chrono::XSystemClock> {
  using WClock_ = ::rex::chrono::WinSystemClock;
  using XClock_ = ::rex::chrono::XSystemClock;

  template <typename Duration>
  typename WClock_::time_point operator()(
      const std::chrono::time_point<XClock_, Duration>& t) const {
    // Consult chrono_steady_cast.h for explanation on this:
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto w_now = WClock_::now();
    auto x_now = XClock_::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    auto delta = (t - x_now);
    if (!REXCVAR_GET(clock_no_scaling)) {
      delta =
          std::chrono::floor<WClock_::duration>(delta * rex::chrono::Clock::guest_time_scalar());
    }
    return w_now + delta;
  }
};

template <>
struct clock_time_conversion<::rex::chrono::XSystemClock, ::rex::chrono::WinSystemClock> {
  using WClock_ = ::rex::chrono::WinSystemClock;
  using XClock_ = ::rex::chrono::XSystemClock;

  template <typename Duration>
  typename XClock_::time_point operator()(
      const std::chrono::time_point<WClock_, Duration>& t) const {
    // Consult chrono_steady_cast.h for explanation on this:
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto w_now = WClock_::now();
    auto x_now = XClock_::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    rex::chrono::hundrednanoseconds delta = (t - w_now);
    if (!REXCVAR_GET(clock_no_scaling)) {
      delta =
          std::chrono::floor<WClock_::duration>(delta / rex::chrono::Clock::guest_time_scalar());
    }
    return x_now + delta;
  }
};

}  // namespace std::chrono
