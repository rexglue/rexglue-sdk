#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <sys/time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

namespace rex {

u64 Clock::GetHostTickFrequency() noexcept {
#ifdef __APPLE__
  mach_timebase_info_data_t info;
  mach_timebase_info(&info);
  return static_cast<u64>((1000000000ull * static_cast<u64>(info.denom)) /
                          static_cast<u64>(info.numer));
#else
  timespec res;
  int error = clock_getres(CLOCK_MONOTONIC, &res);
  REX_ASSERT(error == 0);
  // Sub second resolution is required.
  REX_ASSERT(res.tv_sec == 0);

  return 1000000000ull / res.tv_nsec;
#endif
}

u64 Clock::GetHostTickCount() noexcept {
#ifdef __APPLE__
  // Ticks must stay in mach units, since nanoseconds against the 24 MHz frequency run 41x fast.
  return mach_absolute_time();
#else
  timespec tp;
  int error = clock_gettime(CLOCK_MONOTONIC, &tp);
  REX_ASSERT(error == 0);

  return tp.tv_nsec + tp.tv_sec * 1000000000ull;
#endif
}

u64 Clock::QueryHostSystemTime() noexcept {
  constexpr u64 kSecondsPerDay = u64{3600} * 24;
  constexpr u64 kSeconds1601To1970 = ((u64{369} * 365 + 89) * kSecondsPerDay);

  timeval now;
  int error = gettimeofday(&now, nullptr);
  REX_ASSERT(error == 0);

  // FILETIME counts 100ns intervals.
  return static_cast<u64>((static_cast<i64>(now.tv_sec) + kSeconds1601To1970) * 10000000ull +
                          now.tv_usec * 10);
}

u64 Clock::QueryHostUptimeMillis() {
  return GetHostTickCount() * 1000 / GetHostTickFrequency();
}

}  // namespace rex
