/**
 * @file        tests/unit/kernel/identity_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/runtime.h>
#include <rex/runtime/xtypes.h>

#include "kernel_fixture.h"

using rex::X_STATUS;

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {
u32 Poll(Caller& c) {
  return c.Quad(0);
}
}  // namespace

REX_TEST_CASE("identity: the stash survives and survives repeated resolution",
              "[kernel][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event = c.Alloc(0x10);
    c.Kernel("KeInitializeEvent", {event, 0, 0});
    const u32 stashed = c.Load32(event + 12);
    CHECK(stashed >= 0xF8000004);
    CHECK(c.Kernel("KeSetEvent", {event, 0, 0}) == 1);
    CHECK(c.Load32(event + 12) == stashed);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {stashed, 1, 0, Poll(c)}) == 0);
  });
}

REX_TEST_CASE(
    "identity: ObReferenceObjectByHandle returns the same guest pointer for the same object",
    "[kernel][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 first = c.Alloc(4);
    const u32 second = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    CHECKED_ELSE(c.Kernel("ObReferenceObjectByHandle", {handle, 0, first}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    CHECKED_ELSE(c.Kernel("ObReferenceObjectByHandle", {handle, 0, second}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    CHECK(c.Load32(first) == c.Load32(second));
    c.Kernel("ObDereferenceObject", {c.Load32(first)});
    c.Kernel("ObDereferenceObject", {c.Load32(second)});
    c.Kernel("NtClose", {handle});
  });
}

REX_TEST_CASE("identity: the current thread pseudo handle", "[kernel][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 copy_ptr = c.Alloc(4);
    CHECK(c.Kernel("NtDuplicateObject", {0xFFFFFFFE, copy_ptr, 0}) == 0);
    const u32 copy = c.Load32(copy_ptr);
    CHECK(copy >= 0xF8000004);
    const u32 object_ptr = c.Alloc(4);
    CHECK(c.Kernel("ObReferenceObjectByHandle", {0xFFFFFFFE, 0, object_ptr}) == 0);
    CHECK(c.Load32(object_ptr) != 0);
    c.Kernel("ObDereferenceObject", {c.Load32(object_ptr)});
    CHECK(c.Kernel("NtClose", {copy}) == 0);
  });
}
