/**
 * @file        tests/unit/kernel/irql_benchmark_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <catch2/benchmark/catch_benchmark.hpp>

#include <rex/ppc/recompiled_code.h>

#include "benchmark_pool.h"
#include "kernel_fixture.h"
#include "kernel/dispatcher.h"
#include "kernel/title_state.h"

using rex::test::Caller;
using rex::test::KernelFixture;
using rex::test::ThreadPool;

namespace {

constexpr int kPairsPerThread = 5000;
constexpr int kThreads = 4;

/** An export that does nothing, for the baseline of the call path. */
REX_FUNC(NoOp) {}

/** Raises and lowers the IRQL pairs times with the exports found once by the caller. */
void RaiseAndLower(Caller& c, const Caller::ResolvedExport& raise,
                   const Caller::ResolvedExport& lower, int pairs) {
  for (int i = 0; i < pairs; ++i) {
    lower(c, {raise(c)});
  }
}

bool HasQueuedDPCs(Caller& c) {
  return static_cast<rex::kernel::Dispatcher&>(StateOf(c.GetRuntime()).GetDPCQueue())
      .HasQueuedDPCs();
}

}  // namespace

REX_TEST_CASE("Raising and lowering the IRQL with an empty DPC queue", "[kernel][dpc][benchmark]") {
  KernelFixture fixture;
  fixture.RunBenchmarkOnGuestThread([](Caller& c) {
    CHECK_FALSE(HasQueuedDPCs(c));
    const auto raise = Caller::Resolve("KeRaiseIrqlToDpcLevel");
    const auto lower = Caller::Resolve("KfLowerIrql");
    const auto nothing = Caller::Resolve(&NoOp);
    BENCHMARK("baseline: 2 empty calls through the same path") {
      nothing(c, {0});
      return nothing(c, {0});
    };
    BENCHMARK("raise and lower, 1 pair") {
      RaiseAndLower(c, raise, lower, 1);
      return 0;
    };
    BENCHMARK("raise and lower, 1 thread, 5000 pairs") {
      RaiseAndLower(c, raise, lower, kPairsPerThread);
      return 0;
    };
    ThreadPool pool(
        kThreads,
        [&raise, &lower](int, Caller* thread) {
          RaiseAndLower(*thread, raise, lower, kPairsPerThread);
        },
        &c);
    ThreadPool idle(kThreads, [](int, Caller*) {}, &c);
    BENCHMARK("baseline: 4 threads released and joined, no pairs") {
      idle.Run();
      return 0;
    };
    BENCHMARK("raise and lower, 4 threads, 5000 pairs each") {
      pool.Run();
      return 0;
    };
    WARN("the 4 thread rounds ran together for " << pool.GetOverlap() * 100.0
                                                 << " percent of the round");
    CHECK_FALSE(HasQueuedDPCs(c));
  });
}
