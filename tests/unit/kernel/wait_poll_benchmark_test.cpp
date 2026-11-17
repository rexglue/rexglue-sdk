/**
 * @file        tests/unit/kernel/wait_poll_benchmark_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <catch2/benchmark/catch_benchmark.hpp>

#include "kernel_fixture.h"

using rex::test::Caller;
using rex::test::KernelFixture;

REX_TEST_CASE("A zero timeout poll of an unsignaled event", "[kernel][waits][benchmark]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event = c.Alloc(0x10);
    c.Kernel("KeInitializeEvent", {event, 0, 0});
    const u32 poll = c.AllocQuad(0);
    BENCHMARK("zero timeout poll, 1 thread") {
      return c.Kernel("KeWaitForSingleObject", {event, 0, 0, 0, poll});
    };
  });
}

REX_TEST_CASE("A wait on a signaled manual reset event", "[kernel][waits][benchmark]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event = c.Alloc(0x10);
    c.Kernel("KeInitializeEvent", {event, 0, 1});
    const u32 poll = c.AllocQuad(0);
    BENCHMARK("wait on a signaled manual reset event, 1 thread") {
      return c.Kernel("KeWaitForSingleObject", {event, 0, 0, 0, poll});
    };
  });
}
