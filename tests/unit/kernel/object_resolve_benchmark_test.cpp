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
#include <vector>

#include <rex/kernel/object/object_table.h>
#include <rex/kernel/title_runtime.h>
#include <rex/platform/lock_order.h>

#include "benchmark_pool.h"
#include "kernel_fixture.h"

using rex::test::Caller;
using rex::test::KernelFixture;
using rex::test::ThreadPool;

namespace {

constexpr int kCallsPerThread = 20000;

/** Reports how long the threads of a pool ran together, averaged over its rounds. */
void ReportOverlap(const char* name, const ThreadPool& pool) {
  WARN(name << ": the threads ran together for " << pool.GetOverlap() * 100.0
            << " percent of the round");
}

/** Registers 8 guest events and resolves them from host threads. */
struct ResolveSetup {
  explicit ResolveSetup(Caller& c) : table(c.GetRuntime().GetObjectTable()) {
    for (int i = 0; i < 8; ++i) {
      const u32 event = c.Alloc(0x10);
      c.Kernel("KeInitializeEvent", {event, 0, 0});
      events.push_back(event);
    }
  }

  /** Resolves the thread's own event, or events[0] for every thread when same is set. */
  ThreadPool::Body ResolveBody(bool same) {
    return [this, same](int index, Caller*) {
      const u32 event = events[same ? 0 : static_cast<size_t>(index)];
      u32 found = 0;
      for (int n = 0; n < kCallsPerThread; ++n) {
        found += table.ResolveObject(event, -1) ? 1 : 0;
      }
      if (found != kCallsPerThread) {
        misses.fetch_add(1, std::memory_order_relaxed);
      }
    };
  }

  rex::kernel::IObjectTable& table;
  std::vector<u32> events;
  std::atomic<u64> misses{0};
};

/** Takes and releases the lock the table takes in ResolveObject, in the same shared mode. */
ThreadPool::Body LockBody(rex::platform::RankedSharedMutex& mutex) {
  return [&mutex](int, Caller*) {
    for (int n = 0; n < kCallsPerThread; ++n) {
      mutex.lock_shared();
      mutex.unlock_shared();
    }
  };
}

}  // namespace

REX_TEST_CASE("ResolveObject on one thread", "[kernel][identity][benchmark]") {
  KernelFixture fixture;
  fixture.RunBenchmarkOnGuestThread([](Caller& c) {
    ResolveSetup setup(c);
    ThreadPool one(1, setup.ResolveBody(false));
    ThreadPool idle_one(1, [](int, Caller*) {});
    BENCHMARK("baseline: 1 thread released and joined, no calls") {
      idle_one.Run();
      return 0;
    };
    BENCHMARK("ResolveObject, 1 call") {
      return setup.table.ResolveObject(setup.events[0], -1) ? 1 : 0;
    };
    BENCHMARK("ResolveObject, 1 thread, own event") {
      one.Run();
      return 0;
    };
    CHECK(setup.misses.load() == 0);
  });
}

REX_TEST_CASE("ResolveObject on 4 and 8 threads", "[kernel][identity][benchmark]") {
  KernelFixture fixture;
  fixture.RunBenchmarkOnGuestThread([](Caller& c) {
    ResolveSetup setup(c);
    ThreadPool four(4, setup.ResolveBody(false));
    ThreadPool eight(8, setup.ResolveBody(false));
    ThreadPool eight_same(8, setup.ResolveBody(true));
    ThreadPool idle_eight(8, [](int, Caller*) {});
    BENCHMARK("baseline: 8 threads released and joined, no calls") {
      idle_eight.Run();
      return 0;
    };
    BENCHMARK("ResolveObject, 4 threads, own event") {
      four.Run();
      return 0;
    };
    ReportOverlap("ResolveObject, 4 threads, own event", four);
    BENCHMARK("ResolveObject, 8 threads, own event") {
      eight.Run();
      return 0;
    };
    ReportOverlap("ResolveObject, 8 threads, own event", eight);
    BENCHMARK("ResolveObject, 8 threads, same event") {
      eight_same.Run();
      return 0;
    };
    ReportOverlap("ResolveObject, 8 threads, same event", eight_same);
    CHECK(setup.misses.load() == 0);
  });
}

REX_TEST_CASE("The object table lock alone", "[kernel][identity][benchmark]") {
  KernelFixture fixture;
  fixture.RunBenchmarkOnGuestThread([](Caller&) {
    rex::platform::RankedSharedMutex mutex(rex::platform::lock_rank::kObjectTable);
    ThreadPool one(1, LockBody(mutex));
    ThreadPool four(4, LockBody(mutex));
    ThreadPool eight(8, LockBody(mutex));
    BENCHMARK("object table lock alone, 1 thread") {
      one.Run();
      return 0;
    };
    BENCHMARK("object table lock alone, 4 threads") {
      four.Run();
      return 0;
    };
    ReportOverlap("object table lock alone, 4 threads", four);
    BENCHMARK("object table lock alone, 8 threads") {
      eight.Run();
      return 0;
    };
    ReportOverlap("object table lock alone, 8 threads", eight);
  });
}
