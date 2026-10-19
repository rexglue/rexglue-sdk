/**
 * @file        chrono/clock.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/clock.h (Ben Vanik, 2022).
 */

#pragma once

#include <chrono>
#include <utility>

#include <rex/export.h>
#include <rex/platform.h>
#include <rex/types.h>

namespace rex {

/**
 * Process-wide host clock queries and the scaled guest clock. Every member is static.
 */
class REX_API Clock {
 public:
  /**
   * Returns the clock.no_scaling cvar.
   *
   * @return The clock.no_scaling cvar. When true, guest time follows host time unscaled.
   */
  static bool IsNoScaling();

  /**
   * Host tick frequency from the platform time source. Prefer GetHostTickFrequency.
   *
   * @return Host ticks per second.
   */
  static u64 GetHostTickFrequencyPlatform() noexcept;

  /**
   * Host tick count from the platform time source. Prefer GetHostTickCount.
   *
   * @return The current host tick count.
   */
  static u64 GetHostTickCountPlatform();

  /**
   * Returns the host tick frequency.
   *
   * @return Host ticks per second from the platform time source.
   */
  static u64 GetHostTickFrequency();

  /**
   * Returns the current host tick count.
   *
   * @return The current host tick count from the platform time source.
   */
  static u64 GetHostTickCount() noexcept;

  /**
   * Returns the host system time.
   *
   * @return The host system time as a FILETIME, in 100ns units since Jan 1 1601.
   */
  static u64 QueryHostSystemTime() noexcept;

  /**
   * Returns the host uptime in milliseconds.
   *
   * @return Milliseconds since the host platform tick source started.
   */
  static u64 QueryHostUptimeMillis();

  /**
   * Returns the guest time scalar.
   *
   * @return The guest time scalar. 1 is normal speed.
   */
  static f64 GetGuestTimeScalar();

  /**
   * Sets the guest time scalar, which scales guest tick and wall clock speed. 2 runs at double
   * speed and 0.5 at half speed. Ignored when no_scaling is true.
   *
   * @param scalar The new scalar.
   */
  static void SetGuestTimeScalar(f64 scalar);

  /**
   * Returns the host to guest tick ratio.
   *
   * @return The host to guest tick ratio with time scaling applied, as a reduced numerator and
   *         denominator pair.
   */
  static std::pair<u64, u64> GetGuestTickRatio();

  /**
   * Returns the guest tick frequency.
   *
   * @return Guest ticks per second.
   */
  static u64 GetGuestTickFrequency();

  /**
   * Sets the guest tick frequency and recomputes the tick ratio.
   *
   * @param frequency Guest ticks per second.
   */
  static void SetGuestTickFrequency(u64 frequency);

  /**
   * Returns the FILETIME base of the guest system time.
   *
   * @return The FILETIME base the guest system time counts from. It defaults to the host system
   *         time at startup.
   */
  static u64 GetGuestSystemTimeBase();

  /**
   * Sets the FILETIME base the guest system time counts from.
   *
   * @param time_base The new base as a FILETIME.
   */
  static void SetGuestSystemTimeBase(u64 time_base);

  /**
   * Advances and reads the guest tick count, with frequency adjustment and scaling applied.
   *
   * @return The current guest tick count.
   */
  static u64 QueryGuestTickCount();

  /**
   * Returns the guest system time.
   *
   * @return The guest system time as a scaled FILETIME. It equals host system time when
   *         no_scaling is true.
   */
  static u64 QueryGuestSystemTime();

  /**
   * Returns the title uptime in milliseconds.
   *
   * @return Scaled milliseconds since the guest system time base, clamped to the u32 range.
   */
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
   * @param guest_ms The guest duration in milliseconds.
   * @return The scaled duration, clamped to the u32 range.
   */
  static u32 ScaleGuestDurationMillis(u32 guest_ms);

  /**
   * Scales a guest FILETIME duration by the time scalar. A positive value is an absolute time and
   * scales relative to the current guest system time. A negative value is a relative duration.
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

}  // namespace rex
