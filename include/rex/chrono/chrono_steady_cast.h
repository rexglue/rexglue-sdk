/**
 * @file        chrono/chrono_steady_cast.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/chrono_steady_cast.h (Ben Vanik, 2022).
 */

#pragma once

#include <atomic>

#include <rex/chrono/chrono.h>

namespace std::chrono {

/**
 * Converts steady_clock time to WinSystemClock time. steady_clock has no fixed epoch, so the
 * conversion samples both clocks and carries the offset, which adds a small error. It covers host
 * time only. Convert XSystemClock time to WinSystemClock first.
 */
template <>
struct clock_time_conversion<::rex::WinSystemClock, std::chrono::steady_clock> {
  /** Destination clock. */
  using WinSystemClock_ = ::rex::WinSystemClock;
  /** Source clock. */
  using steady_clock_ = std::chrono::steady_clock;

  /**
   * @tparam Duration The duration of the source time point.
   * @param t The steady_clock time point.
   * @return The same instant on WinSystemClock.
   */
  template <typename Duration>
  typename WinSystemClock_::time_point operator()(
      const std::chrono::time_point<steady_clock_, Duration>& t) const {
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto steady_now = steady_clock_::now();
    auto nt_now = WinSystemClock_::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    auto delta = std::chrono::floor<WinSystemClock_::duration>(t - steady_now);
    return nt_now + delta;
  }
};

/**
 * Converts WinSystemClock time to steady_clock time by sampling both clocks and carrying the
 * offset. NT waits use the FILETIME epoch as a steady clock, which this conversion serves.
 */
template <>
struct clock_time_conversion<std::chrono::steady_clock, ::rex::WinSystemClock> {
  /** Source clock. */
  using WinSystemClock_ = ::rex::WinSystemClock;
  /** Destination clock. */
  using steady_clock_ = std::chrono::steady_clock;

  /**
   * @tparam Duration The duration of the source time point.
   * @param t The WinSystemClock time point.
   * @return The same instant on steady_clock.
   */
  template <typename Duration>
  steady_clock_::time_point operator()(
      const std::chrono::time_point<WinSystemClock_, Duration>& t) const {
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto steady_now = steady_clock_::now();
    auto nt_now = WinSystemClock_::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    auto delta = t - nt_now;
    return steady_now + delta;
  }
};

}  // namespace std::chrono
