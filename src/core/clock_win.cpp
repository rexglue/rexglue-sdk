/**
 * @file        core/clock_win.cpp
 * @brief       Windows platform clock implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/chrono/clock.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include "platform_win.h"

namespace rex::chrono {

uint64_t Clock::host_tick_frequency_platform() {
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  return frequency.QuadPart;
}

uint64_t Clock::host_tick_count_platform() {
  LARGE_INTEGER counter;
  uint64_t time = 0;
  if (QueryPerformanceCounter(&counter)) {
    time = counter.QuadPart;
  }
  return time;
}

uint64_t Clock::QueryHostSystemTime() {
  FILETIME t;
  GetSystemTimeAsFileTime(&t);
  return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

uint64_t Clock::QueryHostUptimeMillis() {
  return host_tick_count_platform() * 1000 / host_tick_frequency_platform();
}

}  // namespace rex::chrono
