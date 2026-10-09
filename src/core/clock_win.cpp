#include <rex/chrono/clock.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include "platform_win.h"

namespace rex {

u64 Clock::host_tick_frequency_platform() noexcept {
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  return frequency.QuadPart;
}

u64 Clock::host_tick_count_platform() {
  LARGE_INTEGER counter;
  u64 time = 0;
  if (QueryPerformanceCounter(&counter)) {
    time = counter.QuadPart;
  }
  return time;
}

u64 Clock::QueryHostSystemTime() noexcept {
  FILETIME t;
  GetSystemTimeAsFileTime(&t);
  return (static_cast<u64>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

u64 Clock::QueryHostUptimeMillis() {
  return host_tick_count_platform() * 1000 / host_tick_frequency_platform();
}

}  // namespace rex
