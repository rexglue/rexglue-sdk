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
#include <atomic>
#include <cstdio>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

#include <rex/chrono.h>
#include <rex/math.h>
#include <rex/settings_registry.h>

#include "base/core_settings.h"

namespace rex {

ClockSettings& GetClockSettings() {
  static ClockSettings& settings = [] -> ClockSettings& {
    static ClockSettings owner;
    if (auto registered = REX_SETTINGS().Register(owner); !registered) {
      std::fprintf(stderr, "clock settings are not registered, %s\n",
                   registered.error().message.c_str());
    }
    return owner;
  }();
  return settings;
}

bool Clock::IsNoScaling() {
  return GetClockSettings().no_scaling.Get();
}

namespace {

constexpr u64 kFileTimeTicksPerSecond = 10000000;

constinit std::atomic<f64> g_guest_time_scalar{1.0};
constinit std::atomic<u64> g_guest_tick_frequency{0};
constinit std::atomic<u64> g_resolved_tick_frequency{0};
constinit std::atomic<u64> g_guest_system_time_base{0};

/** The point the guest tick count is computed from, and the ratios that convert from it. */
struct TickState {
  u64 host = 0;
  u64 guest = 0;
  u64 ratio_num = 1;
  u64 ratio_den = 1;
  u64 time_num = 0;
  u64 time_den = 0;
};

struct SharedTickState {
  std::atomic<u64> host{0};
  std::atomic<u64> guest{0};
  std::atomic<u64> ratio_num{1};
  std::atomic<u64> ratio_den{1};
  std::atomic<u64> time_num{0};
  std::atomic<u64> time_den{0};
};

constinit SharedTickState g_tick_state;
constinit std::atomic<u32> g_tick_sequence{0};
constinit std::atomic<u64> g_last_guest_tick{0};
std::mutex g_tick_mutex;

TickState ReadTickState() {
  for (;;) {
    const u32 before = g_tick_sequence.load(std::memory_order_acquire);
    if (before & 1) {
      std::this_thread::yield();
      continue;
    }
    TickState state;
    state.host = g_tick_state.host.load(std::memory_order_relaxed);
    state.guest = g_tick_state.guest.load(std::memory_order_relaxed);
    state.ratio_num = g_tick_state.ratio_num.load(std::memory_order_relaxed);
    state.ratio_den = g_tick_state.ratio_den.load(std::memory_order_relaxed);
    state.time_num = g_tick_state.time_num.load(std::memory_order_relaxed);
    state.time_den = g_tick_state.time_den.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (g_tick_sequence.load(std::memory_order_relaxed) == before) {
      return state;
    }
  }
}

void WriteTickStateLocked(const TickState& state) {
  const u32 sequence = g_tick_sequence.load(std::memory_order_relaxed);
  g_tick_sequence.store(sequence + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  g_tick_state.host.store(state.host, std::memory_order_relaxed);
  g_tick_state.guest.store(state.guest, std::memory_order_relaxed);
  g_tick_state.ratio_num.store(state.ratio_num, std::memory_order_relaxed);
  g_tick_state.ratio_den.store(state.ratio_den, std::memory_order_relaxed);
  g_tick_state.time_num.store(state.time_num, std::memory_order_relaxed);
  g_tick_state.time_den.store(state.time_den, std::memory_order_relaxed);
  g_tick_sequence.store(sequence + 2, std::memory_order_release);
}

u64 GuestTickFrequency() {
  u64 frequency = g_resolved_tick_frequency.load(std::memory_order_relaxed);
  if (frequency == 0) {
    const u64 set = g_guest_tick_frequency.load(std::memory_order_relaxed);
    frequency = set ? set : Clock::GetHostTickFrequency();
    u64 unresolved = 0;
    if (!g_resolved_tick_frequency.compare_exchange_strong(unresolved, frequency,
                                                           std::memory_order_relaxed)) {
      frequency = unresolved;
    }
  }
  return frequency;
}

std::pair<u64, u64> ComputeGuestTickRatio() {
  const f64 scalar = g_guest_time_scalar.load(std::memory_order_relaxed);
  auto frac = std::make_pair(GuestTickFrequency(), Clock::GetHostTickFrequency());
  if (scalar > 1.0) {
    frac.first *= static_cast<u64>(scalar * 10.0);
    frac.second *= 10;
  } else {
    frac.first *= 10;
    frac.second *= static_cast<u64>(10.0 / scalar);
  }
  reduce_fraction(frac);
  return frac;
}

std::pair<u64, u64> ComputeSystemTimeRatio() {
  u64 numerator = kFileTimeTicksPerSecond;
  u64 denominator = GuestTickFrequency();
  reduce_fraction(numerator, denominator);
  return {numerator, denominator};
}

/** Raises the last published guest tick count to at least the given one and returns the result. */
u64 PublishGuestTick(u64 guest) {
  u64 last = g_last_guest_tick.load(std::memory_order_relaxed);
  while (guest > last &&
         !g_last_guest_tick.compare_exchange_weak(last, guest, std::memory_order_relaxed)) {}
  return std::max(guest, last);
}

u64 GuestTickFrom(const TickState& state, u64 host_tick_count) {
  const u64 host_tick_delta = host_tick_count > state.host ? host_tick_count - state.host : 0;
  return state.guest + mul_div(host_tick_delta, state.ratio_num, state.ratio_den);
}

/** Starts the tick state at the first read. Holds g_tick_mutex. */
TickState InitTickStateLocked() {
  TickState state = ReadTickState();
  if (state.host == 0) {
    state.host = Clock::GetHostTickCount();
    state.guest = 0;
    const auto time_ratio = ComputeSystemTimeRatio();
    state.time_num = time_ratio.first;
    state.time_den = time_ratio.second;
    WriteTickStateLocked(state);
  }
  return state;
}

TickState LoadTickState() {
  TickState state = ReadTickState();
  if (state.host == 0) {
    std::lock_guard<std::mutex> lock(g_tick_mutex);
    state = InitTickStateLocked();
  }
  return state;
}

/** Moves the start of the tick state to now with the current ratios, then replaces the ratios. */
void RebaseTickStateLocked() {
  const TickState before = InitTickStateLocked();
  const u64 host = Clock::GetHostTickCount();
  TickState state;
  state.host = host;
  state.guest = PublishGuestTick(GuestTickFrom(before, host));
  const auto ratio = ComputeGuestTickRatio();
  state.ratio_num = ratio.first;
  state.ratio_den = ratio.second;
  const auto time_ratio = ComputeSystemTimeRatio();
  state.time_num = time_ratio.first;
  state.time_den = time_ratio.second;
  WriteTickStateLocked(state);
}

u64 QueryGuestTickFrom(const TickState& state) {
  return PublishGuestTick(GuestTickFrom(state, Clock::GetHostTickCount()));
}

u64 GuestSystemTimeBase() {
  u64 base = g_guest_system_time_base.load(std::memory_order_relaxed);
  if (base == 0) {
    const u64 host_system_time = Clock::QueryHostSystemTime();
    if (g_guest_system_time_base.compare_exchange_strong(base, host_system_time,
                                                         std::memory_order_relaxed)) {
      base = host_system_time;
    }
  }
  return base;
}

u64 GuestSystemTimeOffset(const TickState& state, u64 guest_tick_count) {
  return mul_div(guest_tick_count, state.time_num, state.time_den);
}

u64 QueryGuestSystemTimeOffset() {
  if (GetClockSettings().no_scaling.Get()) {
    return Clock::QueryHostSystemTime() - GuestSystemTimeBase();
  }

  const TickState state = LoadTickState();
  return GuestSystemTimeOffset(state, QueryGuestTickFrom(state));
}
}  // namespace

f64 Clock::GetGuestTimeScalar() {
  return g_guest_time_scalar.load(std::memory_order_relaxed);
}

void Clock::SetGuestTimeScalar(f64 scalar) {
  if (GetClockSettings().no_scaling.Get()) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_tick_mutex);
  g_guest_time_scalar.store(scalar, std::memory_order_relaxed);
  RebaseTickStateLocked();
}

std::pair<u64, u64> Clock::GetGuestTickRatio() {
  const TickState state = ReadTickState();
  return {state.ratio_num, state.ratio_den};
}

u64 Clock::GetGuestTickFrequency() {
  return GuestTickFrequency();
}

void Clock::SetGuestTickFrequency(u64 frequency) {
  std::lock_guard<std::mutex> lock(g_tick_mutex);
  g_guest_tick_frequency.store(frequency, std::memory_order_relaxed);
  g_resolved_tick_frequency.store(frequency ? frequency : Clock::GetHostTickFrequency(),
                                  std::memory_order_relaxed);
  RebaseTickStateLocked();
}

u64 Clock::GetGuestSystemTimeBase() {
  return GuestSystemTimeBase();
}

void Clock::SetGuestSystemTimeBase(u64 time_base) {
  g_guest_system_time_base.store(time_base, std::memory_order_relaxed);
}

u64 Clock::QueryGuestTickCount() {
  if (GetClockSettings().no_scaling.Get()) {
    const u64 host_tick_count = GetHostTickCount();
    const TickState state = ReadTickState();
    return mul_div(host_tick_count, state.ratio_num, state.ratio_den);
  }

  return QueryGuestTickFrom(LoadTickState());
}

u64 Clock::QueryGuestSystemTime() {
  if (GetClockSettings().no_scaling.Get()) {
    return Clock::QueryHostSystemTime();
  }

  const TickState state = LoadTickState();
  return GuestSystemTimeBase() + GuestSystemTimeOffset(state, QueryGuestTickFrom(state));
}

u32 Clock::GetGuestUptimeMillis() {
  return static_cast<u32>(
      std::min<u64>(QueryGuestSystemTimeOffset() / 10000, std::numeric_limits<u32>::max()));
}

void Clock::SetGuestSystemTime(u64 system_time) {
  if (GetClockSettings().no_scaling.Get()) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_tick_mutex);
  const TickState state = InitTickStateLocked();
  const u64 offset = GuestSystemTimeOffset(state, QueryGuestTickFrom(state));
  g_guest_system_time_base.store(system_time - offset, std::memory_order_relaxed);
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
    const TickState state = LoadTickState();
    const u64 guest_time =
        GuestSystemTimeBase() + GuestSystemTimeOffset(state, QueryGuestTickFrom(state));
    i64 relative_time = guest_file_time - static_cast<i64>(guest_time);
    i64 scaled_time = static_cast<i64>(static_cast<f64>(relative_time) * GetGuestTimeScalar());
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
