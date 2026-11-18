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

#include <rex/ppc/recompiled_code.h>

#include "kernel_fixture.h"

using rex::test::Caller;
using rex::test::KernelFixture;

namespace {

/** An export that does nothing, for the baseline of the call path. */
REX_FUNC(NoOp) {}

}  // namespace

REX_TEST_CASE("A zero timeout poll of an unsignaled event", "[kernel][waits][benchmark]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event = c.Alloc(0x10);
    c.Kernel("KeInitializeEvent", {event, 0, 0});
    const u32 poll = c.AllocQuad(0);
    const auto wait = Caller::Resolve("KeWaitForSingleObject");
    const auto nothing = Caller::Resolve(&NoOp);
    BENCHMARK("baseline: 1 empty call through the same path") {
      return nothing(c, {event, 0, 0, 0, poll});
    };
    BENCHMARK("zero timeout poll, 1 thread") {
      return wait(c, {event, 0, 0, 0, poll});
    };
  });
}

REX_TEST_CASE("A wait on a signaled manual reset event", "[kernel][waits][benchmark]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event = c.Alloc(0x10);
    c.Kernel("KeInitializeEvent", {event, 0, 1});
    const u32 poll = c.AllocQuad(0);
    const auto wait = Caller::Resolve("KeWaitForSingleObject");
    const auto nothing = Caller::Resolve(&NoOp);
    BENCHMARK("baseline: 1 empty call through the same path") {
      return nothing(c, {event, 0, 0, 0, poll});
    };
    BENCHMARK("wait on a signaled manual reset event, 1 thread") {
      return wait(c, {event, 0, 0, 0, poll});
    };
  });
}
