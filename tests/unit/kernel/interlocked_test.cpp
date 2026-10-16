/**
 * @file        tests/unit/kernel/interlocked_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <memory>

#include "kernel_fixture.h"

using rex::u32;
using rex::testing::Caller;
using rex::testing::KernelFixture;

REX_TEST_CASE(
    "InterlockedPushEntrySList returns the old head and Pop returns entries last in first out",
    "[kernel][interlocked]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 header = c.Alloc(8);
    const u32 first = c.Alloc(8);
    const u32 second = c.Alloc(8);
    const u32 third = c.Alloc(8);
    CHECK(c.Kernel("InterlockedPushEntrySList", {header, first}) == 0);
    CHECK(c.Kernel("InterlockedPushEntrySList", {header, second}) == first);
    CHECK(c.Kernel("InterlockedPushEntrySList", {header, third}) == second);
    CHECK(c.Kernel("InterlockedPopEntrySList", {header}) == third);
    CHECK(c.Kernel("InterlockedPopEntrySList", {header}) == second);
    CHECK(c.Kernel("InterlockedPopEntrySList", {header}) == first);
    CHECK(c.Kernel("InterlockedPopEntrySList", {header}) == 0);
  });
}

REX_TEST_CASE("InterlockedFlushSList returns the head and empties the list",
              "[kernel][interlocked]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 header = c.Alloc(8);
    const u32 first = c.Alloc(8);
    const u32 second = c.Alloc(8);
    c.Kernel("InterlockedPushEntrySList", {header, first});
    c.Kernel("InterlockedPushEntrySList", {header, second});
    CHECK(c.Kernel("InterlockedFlushSList", {header}) == second);
    CHECK(c.Kernel("InterlockedPopEntrySList", {header}) == 0);
  });
}

REX_TEST_CASE("A critical section counts recursion and excludes another thread",
              "[kernel][interlocked]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 section = c.Alloc(28);
    c.Kernel("RtlInitializeCriticalSection", {section});
    c.Kernel("RtlEnterCriticalSection", {section});
    c.Kernel("RtlEnterCriticalSection", {section});
    CHECK(c.Kernel("RtlTryEnterCriticalSection", {section}) == 1);

    auto other = std::make_shared<std::atomic<int>>(-1);
    const u32 thread = c.Spawn([section, other](Caller& t) {
      other->store(static_cast<int>(t.Kernel("RtlTryEnterCriticalSection", {section})));
    });
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.Quad(-50000000)}) == 0);
    c.Kernel("NtClose", {thread});
    CHECK(other->load() == 0);

    for (int i = 0; i < 3; ++i) {
      c.Kernel("RtlLeaveCriticalSection", {section});
    }
    CHECK(c.Kernel("RtlTryEnterCriticalSection", {section}) == 1);
    c.Kernel("RtlLeaveCriticalSection", {section});
  });
}
