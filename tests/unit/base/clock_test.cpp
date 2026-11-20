/**
 * @file        tests/unit/base/clock_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <atomic>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

#include <rex/chrono.h>

#include "benchmark_pool.h"

namespace {

struct EarlyClockReads {
  rex::u64 tick_frequency;
  rex::u64 system_time;
  rex::u64 tick_count;

  EarlyClockReads() noexcept
      : tick_frequency(rex::Clock::GetGuestTickFrequency()),
        system_time(rex::Clock::QueryGuestSystemTime()),
        tick_count(rex::Clock::QueryGuestTickCount()) {}
};

const EarlyClockReads kEarlyReads;

}  // namespace

REX_TEST_CASE("Clock globals are constant initialized", "[base][clock]") {
  CHECK(kEarlyReads.tick_frequency == rex::Clock::GetHostTickFrequency());
  CHECK(kEarlyReads.system_time != 0);
  CHECK(kEarlyReads.tick_count < rex::Clock::GetHostTickFrequency());
}

REX_TEST_CASE("Guest system time tracks the host after the setters run", "[base][clock]") {
  const rex::u64 frequency = rex::Clock::GetGuestTickFrequency();
  const rex::u64 base = rex::Clock::GetGuestSystemTimeBase();
  rex::Clock::SetGuestTickFrequency(50000000);
  rex::Clock::SetGuestSystemTime(rex::Clock::QueryHostSystemTime());
  CHECK(rex::Clock::GetGuestTickFrequency() == 50000000);
  const rex::u64 guest = rex::Clock::QueryGuestSystemTime();
  const rex::u64 host = rex::Clock::QueryHostSystemTime();
  constexpr rex::u64 kWindow = 10000000;
  CHECK(guest + kWindow > host);
  CHECK(guest < host + kWindow);
  rex::Clock::SetGuestTickFrequency(frequency);
  rex::Clock::SetGuestSystemTimeBase(base);
}

REX_TEST_CASE("Guest tick counts never decrease across threads", "[base][clock][threads]") {
  constexpr size_t kThreads = 8;
  constexpr size_t kReads = 50000;
  std::atomic<size_t> backwards{0};
  std::atomic<size_t> below_published{0};
  std::atomic<rex::u64> published{0};
  std::atomic<rex::u64> published_time{0};
  std::atomic<bool> readers_done{false};
  std::atomic<size_t> rebases{0};
  const rex::f64 scalar = rex::Clock::GetGuestTimeScalar();
  const bool rescale = !rex::Clock::IsNoScaling();
  {
    std::jthread retuner([&] {
      for (int i = 0; rescale && !readers_done.load(); ++i) {
        rex::Clock::SetGuestTimeScalar(i % 2 ? 1.0 : 2.0);
        ++rebases;
      }
    });
    std::latch start(kThreads);
    std::vector<std::jthread> threads;
    threads.reserve(kThreads);
    for (size_t t = 0; t < kThreads; ++t) {
      threads.emplace_back([&] {
        start.arrive_and_wait();
        rex::u64 last = 0;
        rex::u64 last_time = 0;
        for (size_t i = 0; i < kReads; ++i) {
          const rex::u64 before = published.load();
          const rex::u64 before_time = published_time.load();
          const rex::u64 count = rex::Clock::QueryGuestTickCount();
          const rex::u64 time = rex::Clock::QueryGuestSystemTime();
          backwards += count < last || time < last_time;
          below_published += count < before || time < before_time;
          rex::u64 seen = published.load();
          while (count > seen && !published.compare_exchange_weak(seen, count)) {}
          rex::u64 seen_time = published_time.load();
          while (time > seen_time && !published_time.compare_exchange_weak(seen_time, time)) {}
          last = count;
          last_time = time;
        }
      });
    }
    threads.clear();
    readers_done = true;
  }
  rex::Clock::SetGuestTimeScalar(scalar);
  CHECK(backwards.load() == 0);
  CHECK(below_published.load() == 0);
  WARN("rebases during the run: " << rebases.load());
  CHECK((!rescale || rebases.load() > 1000));
}

namespace {

struct PollResult {
  rex::u64 guest_ticks;
  rex::u64 expected_ticks;
  rex::u64 calls;
};

/** Reads the guest tick count once per microsecond for one second and returns how far it moved. */
PollResult PollGuestTicksForOneSecond() {
  constexpr rex::u64 kGuestFrequency = 50000000;
  const rex::f64 scalar = rex::Clock::GetGuestTimeScalar();
  const rex::u64 frequency = rex::Clock::GetGuestTickFrequency();
  rex::Clock::SetGuestTimeScalar(1.0);
  rex::Clock::SetGuestTickFrequency(kGuestFrequency);
  const rex::u64 host_frequency = rex::Clock::GetHostTickFrequency();
  const rex::u64 step = std::max<rex::u64>(host_frequency / 1000000, 1);
  const rex::u64 host_start = rex::Clock::GetHostTickCount();
  const rex::u64 guest_start = rex::Clock::QueryGuestTickCount();
  rex::u64 calls = 0;
  rex::u64 next = host_start;
  rex::u64 host_now = host_start;
  while (host_now - host_start < host_frequency) {
    if (host_now >= next) {
      rex::Clock::QueryGuestTickCount();
      ++calls;
      next = host_now + step;
    }
    host_now = rex::Clock::GetHostTickCount();
  }
  const rex::u64 host_end = rex::Clock::GetHostTickCount();
  const rex::u64 guest_end = rex::Clock::QueryGuestTickCount();
  rex::Clock::SetGuestTimeScalar(scalar);
  rex::Clock::SetGuestTickFrequency(frequency);
  const auto expected = static_cast<rex::u64>(static_cast<rex::f64>(host_end - host_start) *
                                              static_cast<rex::f64>(kGuestFrequency) /
                                              static_cast<rex::f64>(host_frequency));
  return {guest_end - guest_start, expected, calls};
}

}  // namespace

REX_TEST_CASE("The guest tick count follows the host under frequent polling", "[base][clock]") {
  if (rex::Clock::IsNoScaling()) {
    SKIP("clock.no_scaling is set");
  }
  const PollResult result = PollGuestTicksForOneSecond();
  CAPTURE(result.guest_ticks, result.expected_ticks, result.calls);
  CHECK(result.calls > 1000);
  CHECK(result.guest_ticks + result.expected_ticks / 1000 >= result.expected_ticks);
  CHECK(result.guest_ticks <= result.expected_ticks + result.expected_ticks / 1000);
}

REX_TEST_CASE("guest tick accuracy", "[base][clock][benchmark]") {
  if (rex::Clock::IsNoScaling()) {
    SKIP("clock.no_scaling is set");
  }
  const PollResult result = PollGuestTicksForOneSecond();
  WARN("guest ticks after one second of polling " << result.guest_ticks << ", expected "
                                                  << result.expected_ticks << ", calls "
                                                  << result.calls);
}

REX_TEST_CASE("The guest time scalar scales durations and the tick ratio", "[base][clock]") {
  if (rex::Clock::IsNoScaling()) {
    SKIP("clock.no_scaling is set");
  }
  const rex::f64 scalar = rex::Clock::GetGuestTimeScalar();
  const rex::u64 frequency = rex::Clock::GetGuestTickFrequency();
  rex::Clock::SetGuestTickFrequency(50000000);
  rex::Clock::SetGuestTimeScalar(2.0);
  CHECK(rex::Clock::GetGuestTimeScalar() == 2.0);
  CHECK(rex::Clock::ScaleGuestDurationMillis(16) == 32);
  const auto ratio = rex::Clock::GetGuestTickRatio();
  CHECK(static_cast<rex::f64>(ratio.first) / static_cast<rex::f64>(ratio.second) ==
        Catch::Approx(100000000.0 / static_cast<rex::f64>(rex::Clock::GetHostTickFrequency())));
  rex::Clock::SetGuestTimeScalar(scalar);
  rex::Clock::SetGuestTickFrequency(frequency);
  CHECK(rex::Clock::GetGuestTickFrequency() == frequency);
}

namespace {

/** Calls query calls times on the thread and keeps the sum. */
void Query(size_t calls, rex::u64 (*query)()) {
  rex::u64 sum = 0;
  for (size_t i = 0; i < calls; ++i) {
    sum += query();
  }
  Catch::Benchmark::keep_memory(&sum);
}

rex::u64 NoQuery() {
  return 1;
}

}  // namespace

REX_TEST_CASE("guest clock cost", "[base][clock][benchmark]") {
  using rex::test::Caller;
  using rex::test::ThreadPool;
  constexpr size_t kCalls = 20000;
  // The threads start before the timed rounds and are released together for each one.
  const auto pool = [](int count, rex::u64 (*query)()) {
    return std::make_unique<ThreadPool>(count, [query](int, Caller*) { Query(kCalls, query); });
  };
  auto none_one = pool(1, NoQuery);
  auto none_four = pool(4, NoQuery);
  auto none_eight = pool(8, NoQuery);
  auto time_one = pool(1, [] { return rex::Clock::QueryGuestSystemTime(); });
  auto time_four = pool(4, [] { return rex::Clock::QueryGuestSystemTime(); });
  auto time_eight = pool(8, [] { return rex::Clock::QueryGuestSystemTime(); });
  auto tick_one = pool(1, [] { return rex::Clock::QueryGuestTickCount(); });
  auto tick_four = pool(4, [] { return rex::Clock::QueryGuestTickCount(); });
  auto tick_eight = pool(8, [] { return rex::Clock::QueryGuestTickCount(); });
  auto frequency_eight = pool(8, [] { return rex::Clock::GetGuestTickFrequency(); });
  auto scale_eight =
      pool(8, [] { return static_cast<rex::u64>(rex::Clock::ScaleGuestDurationMillis(16)); });
  BENCHMARK("baseline: a no-op query, 1 thread, 20000 calls") {
    none_one->Run();
  };
  BENCHMARK("baseline: a no-op query, 4 threads, 20000 calls each") {
    none_four->Run();
  };
  BENCHMARK("baseline: a no-op query, 8 threads, 20000 calls each") {
    none_eight->Run();
  };
  BENCHMARK("QueryGuestSystemTime, 1 thread, 20000 calls") {
    time_one->Run();
  };
  BENCHMARK("QueryGuestSystemTime, 4 threads, 20000 calls each") {
    time_four->Run();
  };
  BENCHMARK("QueryGuestSystemTime, 8 threads, 20000 calls each") {
    time_eight->Run();
  };
  BENCHMARK("QueryGuestTickCount, 1 thread, 20000 calls") {
    tick_one->Run();
  };
  BENCHMARK("QueryGuestTickCount, 4 threads, 20000 calls each") {
    tick_four->Run();
  };
  BENCHMARK("QueryGuestTickCount, 8 threads, 20000 calls each") {
    tick_eight->Run();
  };
  BENCHMARK("GetGuestTickFrequency, 8 threads, 20000 calls each") {
    frequency_eight->Run();
  };
  BENCHMARK("ScaleGuestDurationMillis, 8 threads, 20000 calls each") {
    scale_eight->Run();
  };
  WARN("4 thread QueryGuestSystemTime rounds ran together for " << time_four->GetOverlap() * 100.0
                                                                << " percent of the round");
  WARN("4 thread QueryGuestTickCount rounds ran together for " << tick_four->GetOverlap() * 100.0
                                                               << " percent of the round");
  WARN("8 thread QueryGuestSystemTime rounds ran together for " << time_eight->GetOverlap() * 100.0
                                                                << " percent of the round");
  WARN("8 thread QueryGuestTickCount rounds ran together for " << tick_eight->GetOverlap() * 100.0
                                                               << " percent of the round");
}
