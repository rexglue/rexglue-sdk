/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <limits>
#include <mutex>

#include <rex/chrono.h>
#include <rex/math.h>
#include <rex/settings_registry.h>

#include "base/core_settings.h"

namespace rex {

ClockSettings& GetClockSettings() {
  static ClockSettings& settings = [] -> ClockSettings& {
    static ClockSettings owner;
    (void)REX_SETTINGS().Register(owner);
    return owner;
  }();
  return settings;
}

bool Clock::IsNoScaling() {
  return GetClockSettings().no_scaling.Get();
}

namespace {

constexpr u64 kFileTimeTicksPerSecond = 10000000;

f64 g_guest_time_scalar = 1.0;
constinit u64 g_guest_tick_frequency = 0;
constinit u64 g_guest_system_time_base = 0;
constinit std::pair<u64, u64> g_guest_tick_ratio{1, 1};

u64 g_last_guest_tick_count = 0;
constinit u64 g_last_host_tick_count = 0;
std::mutex g_tick_mutex;

u64 GuestSystemTimeBaseLocked() {
  if (g_guest_system_time_base == 0) {
    g_guest_system_time_base = Clock::QueryHostSystemTime();
  }
  return g_guest_system_time_base;
}

u64 GuestTickFrequencyLocked() {
  return g_guest_tick_frequency ? g_guest_tick_frequency : Clock::GetHostTickFrequency();
}

void RecomputeGuestTickRatioLocked() {
  auto frac = std::make_pair(GuestTickFrequencyLocked(), Clock::GetHostTickFrequency());
  if (g_guest_time_scalar > 1.0) {
    frac.first *= static_cast<u64>(g_guest_time_scalar * 10.0);
    frac.second *= 10;
  } else {
    frac.first *= 10;
    frac.second *= static_cast<u64>(10.0 / g_guest_time_scalar);
  }
  reduce_fraction(frac);
  g_guest_tick_ratio = frac;
}

std::unique_lock<std::mutex> LockAndUpdateGuestClock() {
  const u64 host_tick_count = Clock::GetHostTickCount();
  std::unique_lock<std::mutex> lock(g_tick_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    lock.lock();
    return lock;
  }
  if (g_last_host_tick_count == 0) {
    g_last_host_tick_count = host_tick_count;
  }
  const u64 host_tick_delta =
      host_tick_count > g_last_host_tick_count ? host_tick_count - g_last_host_tick_count : 0;
  g_last_host_tick_count = host_tick_count;
  g_last_guest_tick_count +=
      mul_div(host_tick_delta, g_guest_tick_ratio.first, g_guest_tick_ratio.second);
  return lock;
}

u64 GuestSystemTimeOffsetLocked() {
  u64 numerator = kFileTimeTicksPerSecond;
  u64 denominator = GuestTickFrequencyLocked();
  reduce_fraction(numerator, denominator);
  return mul_div(g_last_guest_tick_count, numerator, denominator);
}

u64 QueryGuestSystemTimeOffset() {
  if (GetClockSettings().no_scaling.Get()) {
    const u64 host_system_time = Clock::QueryHostSystemTime();
    std::lock_guard<std::mutex> lock(g_tick_mutex);
    return host_system_time - GuestSystemTimeBaseLocked();
  }

  const auto lock = LockAndUpdateGuestClock();
  return GuestSystemTimeOffsetLocked();
}
}  // namespace

f64 Clock::GetGuestTimeScalar() {
  std::lock_guard<std::mutex> lock(g_tick_mutex);
  return g_guest_time_scalar;
}

void Clock::SetGuestTimeScalar(f64 scalar) {
  if (GetClockSettings().no_scaling.Get()) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_tick_mutex);
  g_guest_time_scalar = scalar;
  RecomputeGuestTickRatioLocked();
}

std::pair<u64, u64> Clock::GetGuestTickRatio() {
  std::lock_guard<std::mutex> lock(g_tick_mutex);
  return g_guest_tick_ratio;
}

u64 Clock::GetGuestTickFrequency() {
  std::lock_guard<std::mutex> lock(g_tick_mutex);
  return GuestTickFrequencyLocked();
}

void Clock::SetGuestTickFrequency(u64 frequency) {
  std::lock_guard<std::mutex> lock(g_tick_mutex);
  g_guest_tick_frequency = frequency;
  RecomputeGuestTickRatioLocked();
}

u64 Clock::GetGuestSystemTimeBase() {
  std::lock_guard<std::mutex> lock(g_tick_mutex);
  return GuestSystemTimeBaseLocked();
}

void Clock::SetGuestSystemTimeBase(u64 time_base) {
  std::lock_guard<std::mutex> lock(g_tick_mutex);
  g_guest_system_time_base = time_base;
}

u64 Clock::QueryGuestTickCount() {
  if (GetClockSettings().no_scaling.Get()) {
    const u64 host_tick_count = GetHostTickCount();
    std::lock_guard<std::mutex> lock(g_tick_mutex);
    return mul_div(host_tick_count, g_guest_tick_ratio.first, g_guest_tick_ratio.second);
  }

  const auto lock = LockAndUpdateGuestClock();
  return g_last_guest_tick_count;
}

u64 Clock::QueryGuestSystemTime() {
  if (GetClockSettings().no_scaling.Get()) {
    return Clock::QueryHostSystemTime();
  }

  const auto lock = LockAndUpdateGuestClock();
  return GuestSystemTimeBaseLocked() + GuestSystemTimeOffsetLocked();
}

u32 Clock::GetGuestUptimeMillis() {
  return static_cast<u32>(
      std::min<u64>(QueryGuestSystemTimeOffset() / 10000, std::numeric_limits<u32>::max()));
}

void Clock::SetGuestSystemTime(u64 system_time) {
  if (GetClockSettings().no_scaling.Get()) {
    return;
  }

  const auto lock = LockAndUpdateGuestClock();
  g_guest_system_time_base = system_time - GuestSystemTimeOffsetLocked();
}

u32 Clock::ScaleGuestDurationMillis(u32 guest_ms) {
  if (GetClockSettings().no_scaling.Get()) {
    return guest_ms;
  }

  constexpr u64 kMax = std::numeric_limits<u32>::max();

  if (guest_ms >= kMax) {
    return kMax;
  } else if (!guest_ms) {
    return 0;
  }
  u64 scaled_ms = static_cast<u64>(static_cast<f64>(guest_ms) * GetGuestTimeScalar());
  return static_cast<u32>(std::min(scaled_ms, kMax));
}

i64 Clock::ScaleGuestDurationFileTime(i64 guest_file_time) {
  if (GetClockSettings().no_scaling.Get()) {
    return guest_file_time;
  }

  if (!guest_file_time) {
    return 0;
  } else if (guest_file_time > 0) {
    const auto lock = LockAndUpdateGuestClock();
    const u64 guest_time = GuestSystemTimeBaseLocked() + GuestSystemTimeOffsetLocked();
    i64 relative_time = guest_file_time - static_cast<i64>(guest_time);
    i64 scaled_time = static_cast<i64>(static_cast<f64>(relative_time) * g_guest_time_scalar);
    return static_cast<i64>(guest_time) + scaled_time;
  } else {
    const i64 scaled_file_time =
        static_cast<i64>(static_cast<f64>(guest_file_time) * GetGuestTimeScalar());
    return scaled_file_time;
  }
}

void Clock::ScaleGuestDurationTimeval(i32* tv_sec, i32* tv_usec) {
  if (GetClockSettings().no_scaling.Get()) {
    return;
  }

  constexpr i64 kMicrosPerSecond = 1'000'000;
  const i64 total = i64{*tv_sec} * kMicrosPerSecond + i64{*tv_usec};
  const i64 scaled = static_cast<i64>(static_cast<f64>(total) * GetGuestTimeScalar());
  i64 scaled_sec = scaled / kMicrosPerSecond;
  i64 scaled_usec = scaled % kMicrosPerSecond;
  if (scaled_usec < 0) {
    scaled_usec += kMicrosPerSecond;
    --scaled_sec;
  }
  *tv_sec = static_cast<i32>(scaled_sec);
  *tv_usec = static_cast<i32>(scaled_usec);
}

}  // namespace rex
