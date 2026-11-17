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

#include <atomic>
#include <latch>
#include <thread>
#include <vector>

#include <rex/chrono.h>

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
  rex::Clock::SetGuestSystemTimeBase(rex::Clock::QueryHostSystemTime());
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
  {
    std::latch start(kThreads);
    std::vector<std::jthread> threads;
    threads.reserve(kThreads);
    for (size_t t = 0; t < kThreads; ++t) {
      threads.emplace_back([&] {
        start.arrive_and_wait();
        rex::u64 last = 0;
        rex::u64 last_time = 0;
        for (size_t i = 0; i < kReads; ++i) {
          const rex::u64 count = rex::Clock::QueryGuestTickCount();
          const rex::u64 time = rex::Clock::QueryGuestSystemTime();
          backwards += count < last || time < last_time;
          last = count;
          last_time = time;
        }
      });
    }
  }
  CHECK(backwards.load() == 0);
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

void QueryOnThreads(size_t threads, size_t calls, rex::u64 (*query)()) {
  std::latch start(static_cast<std::ptrdiff_t>(threads));
  std::vector<std::jthread> workers;
  workers.reserve(threads);
  for (size_t t = 0; t < threads; ++t) {
    workers.emplace_back([&start, calls, query] {
      start.arrive_and_wait();
      rex::u64 sum = 0;
      for (size_t i = 0; i < calls; ++i) {
        sum += query();
      }
      Catch::Benchmark::keep_memory(&sum);
    });
  }
}

}  // namespace

REX_TEST_CASE("guest clock cost", "[base][clock][benchmark]") {
  BENCHMARK("QueryGuestSystemTime, 1 thread, 20000 calls") {
    QueryOnThreads(1, 20000, [] { return rex::Clock::QueryGuestSystemTime(); });
  };
  BENCHMARK("QueryGuestSystemTime, 8 threads, 20000 calls each") {
    QueryOnThreads(8, 20000, [] { return rex::Clock::QueryGuestSystemTime(); });
  };
  BENCHMARK("QueryGuestTickCount, 1 thread, 20000 calls") {
    QueryOnThreads(1, 20000, [] { return rex::Clock::QueryGuestTickCount(); });
  };
  BENCHMARK("QueryGuestTickCount, 8 threads, 20000 calls each") {
    QueryOnThreads(8, 20000, [] { return rex::Clock::QueryGuestTickCount(); });
  };
  BENCHMARK("GetGuestTickFrequency, 8 threads, 20000 calls each") {
    QueryOnThreads(8, 20000, [] { return rex::Clock::GetGuestTickFrequency(); });
  };
  BENCHMARK("ScaleGuestDurationMillis, 8 threads, 20000 calls each") {
    QueryOnThreads(8, 20000,
                   [] { return static_cast<rex::u64>(rex::Clock::ScaleGuestDurationMillis(16)); });
  };
}
