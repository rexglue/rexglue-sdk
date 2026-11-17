/**
 * @file        tests/unit/kernel/object_resolve_benchmark_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <catch2/benchmark/catch_benchmark.hpp>

#include <atomic>
#include <shared_mutex>
#include <thread>
#include <vector>

#include <rex/kernel/object/object_table.h>
#include <rex/kernel/title_runtime.h>

#include "kernel_fixture.h"

using rex::test::Caller;
using rex::test::KernelFixture;

namespace {

constexpr int kCallsPerThread = 20000;

/**
 * Resolves a registered event kCallsPerThread times on each of count host threads.
 *
 * @param same True when every thread resolves events[0], false when thread i resolves events[i].
 */
u32 ResolveOnThreads(rex::kernel::IObjectTable& table, const std::vector<u32>& events, int count,
                     bool same = false) {
  std::atomic<u32> resolved{0};
  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    threads.emplace_back([&table, &events, &resolved, i, same] {
      u32 found = 0;
      const u32 event = events[same ? 0 : static_cast<size_t>(i)];
      for (int n = 0; n < kCallsPerThread; ++n) {
        found += table.ResolveObject(event, -1) ? 1 : 0;
      }
      resolved.fetch_add(found);
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  return resolved.load();
}

/** Takes and releases one shared lock kCallsPerThread times on each of count host threads. */
u32 SharedLockOnThreads(std::shared_mutex& mutex, int count) {
  std::atomic<u32> taken{0};
  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    threads.emplace_back([&mutex, &taken] {
      for (int n = 0; n < kCallsPerThread; ++n) {
        mutex.lock_shared();
        mutex.unlock_shared();
      }
      taken.fetch_add(1);
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  return taken.load();
}

}  // namespace

REX_TEST_CASE("ResolveObject on a registered guest event", "[kernel][identity][benchmark]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto& table = c.GetRuntime().GetObjectTable();
    std::vector<u32> events;
    for (int i = 0; i < 8; ++i) {
      const u32 event = c.Alloc(0x10);
      c.Kernel("KeInitializeEvent", {event, 0, 0});
      events.push_back(event);
    }
    BENCHMARK("ResolveObject, 1 call") {
      return table.ResolveObject(events[0], -1) ? 1 : 0;
    };
    BENCHMARK("ResolveObject, 1 thread, own event") {
      return ResolveOnThreads(table, events, 1);
    };
    BENCHMARK("ResolveObject, 4 threads, own event") {
      return ResolveOnThreads(table, events, 4);
    };
    BENCHMARK("ResolveObject, 8 threads, own event") {
      return ResolveOnThreads(table, events, 8);
    };
    BENCHMARK("ResolveObject, 8 threads, same event") {
      return ResolveOnThreads(table, events, 8, true);
    };
    std::shared_mutex mutex;
    BENCHMARK("shared lock alone, 1 thread") {
      return SharedLockOnThreads(mutex, 1);
    };
    BENCHMARK("shared lock alone, 4 threads") {
      return SharedLockOnThreads(mutex, 4);
    };
    BENCHMARK("shared lock alone, 8 threads") {
      return SharedLockOnThreads(mutex, 8);
    };
  });
}
