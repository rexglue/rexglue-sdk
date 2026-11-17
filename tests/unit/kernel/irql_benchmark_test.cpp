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

#include <vector>

#include "kernel_fixture.h"

using rex::test::Caller;
using rex::test::KernelFixture;

namespace {

constexpr int kPairsPerThread = 5000;

void RaiseAndLower(Caller& c, int pairs) {
  for (int i = 0; i < pairs; ++i) {
    const u32 old_irql = c.Kernel("KeRaiseIrqlToDpcLevel");
    c.Kernel("KfLowerIrql", {old_irql});
  }
}

}  // namespace

REX_TEST_CASE("Raising and lowering the IRQL with an empty DPC queue", "[kernel][dpc][benchmark]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    BENCHMARK("raise and lower, 1 pair") {
      RaiseAndLower(c, 1);
      return 0;
    };
    BENCHMARK("raise and lower, 1 thread, 5000 pairs") {
      RaiseAndLower(c, kPairsPerThread);
      return 0;
    };
    BENCHMARK("raise and lower, 4 threads, 5000 pairs each") {
      std::vector<u32> threads;
      threads.reserve(4);
      for (int i = 0; i < 4; ++i) {
        threads.push_back(c.Spawn([](Caller& t) { RaiseAndLower(t, kPairsPerThread); }));
      }
      for (const u32 thread : threads) {
        c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.AllocQuad(-100000000)});
        c.Kernel("NtClose", {thread});
      }
      return 0;
    };
  });
}
