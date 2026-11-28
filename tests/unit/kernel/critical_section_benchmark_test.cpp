/**
 * @file        tests/unit/kernel/critical_section_benchmark_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <rex/kernel/export.h>
#include <rex/platform/perf.h>

#include "kernel_fixture.h"

using rex::test::Caller;
using rex::test::Join;
using rex::test::KernelFixture;

namespace {

constexpr bool kDebug = std::string_view(REXGLUE_BUILD_CONFIG) == "Debug";
constexpr int kRuns = kDebug ? 1 : 11;
constexpr u32 kOpsPerThread = kDebug ? 1000 : 10000;
constexpr u32 kLockCount = 16;

/** Calls one kernel export through its entry point, resolved once outside the timed loops. */
class Export {
 public:
  explicit Export(std::string_view name) {
    for (const auto& entry : rex::kernel::GetExports(rex::kernel::ExportModule::kXboxkrnl)) {
      if (entry.name == name) {
        fn_ = entry.fn;
      }
    }
    CHECK(fn_ != nullptr);
  }

  u32 operator()(Caller& c, u64 r3 = 0, u64 r4 = 0, u64 r5 = 0, u64 r6 = 0, u64 r7 = 0) const {
    PPCContext& ctx = c.GetContext();
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    ctx.r6.u64 = r6;
    ctx.r7.u64 = r7;
    fn_(ctx, c.GetBase());
    return ctx.r3.u32;
  }

 private:
  ::PPCFunc* fn_ = nullptr;
};

struct Exports {
  Export enter{"RtlEnterCriticalSection"};
  Export leave{"RtlLeaveCriticalSection"};
  Export set_event{"KeSetEvent"};
  Export wait{"KeWaitForSingleObject"};
};

/** One timed run of threads released together. */
struct Run {
  std::chrono::nanoseconds elapsed{};
  u32 contended = 0;
  bool overlapped = true;
};

/**
 * Starts thread_count guest threads, parks them on a spin until all are ready, releases them
 * together and times from the release to the last finish. Thread start is outside the time.
 *
 * @param body Runs on each thread with its index, returns how many of its operations were
 *        contended.
 */
Run TimeThreads(Caller& c, int thread_count, const std::function<u32(Caller&, int)>& body) {
  using clock = std::chrono::steady_clock;
  struct Shared {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<u32> contended{0};
    std::array<clock::time_point, 8> starts{};
    std::array<clock::time_point, 8> ends{};
  };
  auto shared = std::make_shared<Shared>();
  std::vector<u32> threads;
  threads.reserve(static_cast<size_t>(thread_count));
  for (int i = 0; i < thread_count; ++i) {
    threads.push_back(c.Spawn([shared, body, i](Caller& t) {
      shared->ready.fetch_add(1);
      while (!shared->go.load(std::memory_order_acquire)) {}
      shared->starts[i] = clock::now();
      shared->contended.fetch_add(body(t, i));
      shared->ends[i] = clock::now();
    }));
  }
  while (shared->ready.load() != thread_count) {
    std::this_thread::yield();
  }
  const auto start = clock::now();
  shared->go.store(true, std::memory_order_release);
  for (const u32 thread : threads) {
    Join(c, thread);
  }
  Run run;
  const auto last = *std::max_element(shared->ends.begin(), shared->ends.begin() + thread_count);
  const auto latest_start =
      *std::max_element(shared->starts.begin(), shared->starts.begin() + thread_count);
  const auto earliest_end =
      *std::min_element(shared->ends.begin(), shared->ends.begin() + thread_count);
  run.elapsed = last - start;
  run.contended = shared->contended.load();
  run.overlapped = thread_count == 1 || latest_start < earliest_end;
  return run;
}

std::optional<rex::platform::CounterID> LiveCounter(std::string_view name) {
  const auto active = rex::platform::FindCounter("active_threads");
  if (!active || rex::platform::GetCounter(*active) == 0) {
    return std::nullopt;
  }
  return rex::platform::FindCounter(name);
}

/**
 * Runs a case kRuns times and prints the median, minimum and maximum time per operation, the
 * contended operations and the waits that blocked over all runs.
 */
void Report(std::string_view name, u64 ops_per_run, const std::function<Run()>& run_once) {
  const auto blocked = LiveCounter("critical_section_blocks");
  const i64 blocked_before = blocked ? rex::platform::GetCounter(*blocked) : 0;
  std::vector<f64> per_op;
  u64 contended = 0;
  bool overlapped = true;
  for (int i = 0; i < kRuns; ++i) {
    const Run run = run_once();
    per_op.push_back(static_cast<f64>(run.elapsed.count()) / static_cast<f64>(ops_per_run));
    contended += run.contended;
    overlapped = overlapped && run.overlapped;
  }
  std::ranges::sort(per_op);
  const std::string counts =
      blocked
          ? std::format("{} waits blocked", rex::platform::GetCounter(*blocked) - blocked_before)
          : std::string("wait counts not built");
  std::cout << std::format(
      "{}: median {:.1f} ns per op, min {:.1f}, max {:.1f}, {} runs, {} of {} ops contended, {}, "
      "threads {}\n",
      name, per_op[per_op.size() / 2], per_op.front(), per_op.back(), kRuns, contended,
      ops_per_run * kRuns, counts, overlapped ? "overlapped" : "did not overlap");
}

/** Returns a body that enters and leaves a section kOpsPerThread times around a guarded count. */
std::function<u32(Caller&, int)> CountUnder(const Exports& exports, u32 section,
                                            const std::shared_ptr<u32>& guarded) {
  return [&exports, guarded, section](Caller& t, int) -> u32 {
    u32 contended = 0;
    for (u32 n = 0; n < kOpsPerThread; ++n) {
      if (static_cast<i32>(t.LoadAcquire32(section + kLockCount)) != -1) {
        ++contended;
      }
      exports.enter(t, section);
      ++*guarded;
      exports.leave(t, section);
    }
    return contended;
  };
}

/** Reports threads counting under one critical section, after a run that checks the count. */
void ReportCriticalSection(std::string_view name, int threads, u32 spin_count) {
  KernelFixture fixture;
  fixture.RunBenchmarkOnGuestThread([name, threads, spin_count](Caller& c) {
    const Exports exports;
    const u32 section = c.Alloc(28);
    c.Kernel("RtlInitializeCriticalSectionAndSpinCount", {section, spin_count});
    auto guarded = std::make_shared<u32>(0);
    const auto body = CountUnder(exports, section, guarded);
    (void)TimeThreads(c, threads, body);
    CHECK(*guarded == static_cast<u32>(threads) * kOpsPerThread);
    Report(name, static_cast<u64>(threads) * kOpsPerThread,
           [&] { return TimeThreads(c, threads, body); });
  });
}

}  // namespace

REX_TEST_CASE("Critical section benchmark: harness with an empty loop", "[kernel][benchmark]") {
  KernelFixture fixture;
  fixture.RunBenchmarkOnGuestThread([](Caller& c) {
    Report("harness, 4 threads, empty loop", u64{4} * kOpsPerThread, [&] {
      return TimeThreads(c, 4, [](Caller&, int) -> u32 {
        volatile u32 sink = 0;
        for (u32 n = 0; n < kOpsPerThread; ++n) {
          sink = sink + 1;
        }
        return 0;
      });
    });
  });
}

REX_TEST_CASE("Critical section benchmark: 1 thread", "[kernel][benchmark]") {
  ReportCriticalSection("1 thread, enter and leave", 1, 0);
}

REX_TEST_CASE("Critical section benchmark: 2 threads", "[kernel][benchmark]") {
  ReportCriticalSection("2 threads, enter and leave, no spin count", 2, 0);
}

REX_TEST_CASE("Critical section benchmark: 4 threads", "[kernel][benchmark]") {
  ReportCriticalSection("4 threads, enter and leave, no spin count", 4, 0);
}

REX_TEST_CASE("Critical section benchmark: 4 threads with a spin count of 4000",
              "[kernel][benchmark]") {
  ReportCriticalSection("4 threads, enter and leave, spin count 4000", 4, 4000);
}

REX_TEST_CASE("Critical section benchmark: event round trips", "[kernel][benchmark]") {
  KernelFixture fixture;
  fixture.RunBenchmarkOnGuestThread([](Caller& c) {
    const Exports exports;
    const u32 ping = c.Alloc(0x10);
    const u32 pong = c.Alloc(0x10);
    c.Kernel("KeInitializeEvent", {ping, 1, 0});
    c.Kernel("KeInitializeEvent", {pong, 1, 0});
    const auto ping_pong = [&exports, ping, pong](Caller& t, int index) -> u32 {
      for (u32 n = 0; n < kOpsPerThread; ++n) {
        if (index == 0) {
          exports.set_event(t, ping, 1, 0);
          exports.wait(t, pong, 0, 0, 0, 0);
        } else {
          exports.wait(t, ping, 0, 0, 0, 0);
          exports.set_event(t, pong, 1, 0);
        }
      }
      return index == 0 ? kOpsPerThread : 0;
    };
    Report("2 threads, round trips through 2 synchronization events", kOpsPerThread,
           [&] { return TimeThreads(c, 2, ping_pong); });
  });
}
