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

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/math.h>

#include "core/cvar_table.h"

namespace rex {

bool Clock::no_scaling() {
  return REXCVAR_GET(::rex::cvars::no_scaling);
}

namespace {

constexpr u64 kFileTimeTicksPerSecond = 10000000;

f64 g_guest_time_scalar = 1.0;
u64 g_guest_tick_frequency = Clock::host_tick_frequency_platform();
u64 g_guest_system_time_base = Clock::QueryHostSystemTime();
std::pair<u64, u64> g_guest_tick_ratio = std::make_pair(1, 1);

u64 g_last_guest_tick_count = 0;
u64 g_last_host_tick_count = Clock::QueryHostTickCount();
std::mutex g_tick_mutex;

void RecomputeGuestTickScalar() {
  auto frac = std::make_pair(g_guest_tick_frequency, Clock::QueryHostTickFrequency());
  if (g_guest_time_scalar > 1.0) {
    frac.first *= static_cast<u64>(g_guest_time_scalar * 10.0);
    frac.second *= 10;
  } else {
    frac.first *= 10;
    frac.second *= static_cast<u64>(10.0 / g_guest_time_scalar);
  }
  reduce_fraction(frac);

  std::lock_guard<std::mutex> lock(g_tick_mutex);
  g_guest_tick_ratio = frac;
}

u64 UpdateGuestClock() {
  u64 host_tick_count = Clock::QueryHostTickCount();

  if (REXCVAR_GET(::rex::cvars::no_scaling)) {
    return host_tick_count * g_guest_tick_ratio.first / g_guest_tick_ratio.second;
  }

  std::unique_lock<std::mutex> lock(g_tick_mutex, std::defer_lock);
  if (lock.try_lock()) {
    u64 host_tick_delta =
        host_tick_count > g_last_host_tick_count ? host_tick_count - g_last_host_tick_count : 0;
    g_last_host_tick_count = host_tick_count;
    u64 guest_tick_delta = host_tick_delta * g_guest_tick_ratio.first / g_guest_tick_ratio.second;
    g_last_guest_tick_count += guest_tick_delta;
    return g_last_guest_tick_count;
  } else {
    lock.lock();
    return g_last_guest_tick_count;
  }
}

inline u64 QueryGuestSystemTimeOffset() {
  if (REXCVAR_GET(::rex::cvars::no_scaling)) {
    return Clock::QueryHostSystemTime() - g_guest_system_time_base;
  }

  auto guest_tick_count = UpdateGuestClock();

  u64 numerator = kFileTimeTicksPerSecond;
  u64 denominator = g_guest_tick_frequency;
  reduce_fraction(numerator, denominator);

  return guest_tick_count * numerator / denominator;
}
}  // namespace

u64 Clock::QueryHostTickFrequency() {
#if REX_CLOCK_RAW_AVAILABLE
  if (REXCVAR_GET(::rex::cvars::source_raw)) {
    return host_tick_frequency_raw();
  }
#endif
  return host_tick_frequency_platform();
}
u64 Clock::QueryHostTickCount() noexcept {
#if REX_CLOCK_RAW_AVAILABLE
  if (REXCVAR_GET(::rex::cvars::source_raw)) {
    return host_tick_count_raw();
  }
#endif
  return host_tick_count_platform();
}

f64 Clock::guest_time_scalar() {
  return g_guest_time_scalar;
}

void Clock::set_guest_time_scalar(f64 scalar) {
  if (REXCVAR_GET(::rex::cvars::no_scaling)) {
    return;
  }

  g_guest_time_scalar = scalar;
  RecomputeGuestTickScalar();
}

std::pair<u64, u64> Clock::guest_tick_ratio() {
  std::lock_guard<std::mutex> lock(g_tick_mutex);
  return g_guest_tick_ratio;
}

u64 Clock::guest_tick_frequency() {
  return g_guest_tick_frequency;
}

void Clock::set_guest_tick_frequency(u64 frequency) {
  g_guest_tick_frequency = frequency;
  RecomputeGuestTickScalar();
}

u64 Clock::guest_system_time_base() {
  return g_guest_system_time_base;
}

void Clock::set_guest_system_time_base(u64 time_base) {
  g_guest_system_time_base = time_base;
}

u64 Clock::QueryGuestTickCount() {
  auto guest_tick_count = UpdateGuestClock();
  return guest_tick_count;
}

u64 Clock::QueryGuestSystemTime() {
  if (REXCVAR_GET(::rex::cvars::no_scaling)) {
    return Clock::QueryHostSystemTime();
  }

  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  return g_guest_system_time_base + guest_system_time_offset;
}

u32 Clock::QueryGuestUptimeMillis() {
  return static_cast<u32>(
      std::min<u64>(QueryGuestSystemTimeOffset() / 10000, std::numeric_limits<u32>::max()));
}

void Clock::SetGuestSystemTime(u64 system_time) {
  if (REXCVAR_GET(::rex::cvars::no_scaling)) {
    return;
  }

  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  g_guest_system_time_base = system_time - guest_system_time_offset;
}

u32 Clock::ScaleGuestDurationMillis(u32 guest_ms) {
  if (REXCVAR_GET(::rex::cvars::no_scaling)) {
    return guest_ms;
  }

  constexpr u64 kMax = std::numeric_limits<u32>::max();

  if (guest_ms >= kMax) {
    return kMax;
  } else if (!guest_ms) {
    return 0;
  }
  u64 scaled_ms = static_cast<u64>(static_cast<f64>(guest_ms) * g_guest_time_scalar);
  return static_cast<u32>(std::min(scaled_ms, kMax));
}

i64 Clock::ScaleGuestDurationFileTime(i64 guest_file_time) {
  if (REXCVAR_GET(::rex::cvars::no_scaling)) {
    return guest_file_time;
  }

  if (!guest_file_time) {
    return 0;
  } else if (guest_file_time > 0) {
    u64 guest_time = Clock::QueryGuestSystemTime();
    i64 relative_time = guest_file_time - static_cast<i64>(guest_time);
    i64 scaled_time = static_cast<i64>(static_cast<f64>(relative_time) * g_guest_time_scalar);
    return static_cast<i64>(guest_time) + scaled_time;
  } else {
    const i64 scaled_file_time =
        static_cast<i64>(static_cast<f64>(guest_file_time) * g_guest_time_scalar);
    return scaled_file_time;
  }
}

void Clock::ScaleGuestDurationTimeval(i32* tv_sec, i32* tv_usec) {
  if (REXCVAR_GET(::rex::cvars::no_scaling)) {
    return;
  }

  constexpr i64 kMicrosPerSecond = 1'000'000;
  const i64 total = i64{*tv_sec} * kMicrosPerSecond + i64{*tv_usec};
  const i64 scaled = static_cast<i64>(static_cast<f64>(total) * g_guest_time_scalar);
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
