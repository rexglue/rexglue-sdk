/**
 * @file        tests/unit/kernel/notify_listener_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/runtime.h>
#include <rex/runtime/notifier.h>

#include "kernel_fixture.h"

using rex::test::Caller;
using rex::test::KernelFixture;

REX_TEST_CASE("notifications: listener receives a matching broadcast", "[kernel][notify]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 listener = c.XAM("XamNotifyCreateListener", {~0ull, 0});
    CHECKED_ELSE(listener != 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 id_ptr = c.Alloc(4);
    const u32 param_ptr = c.Alloc(4);
    while (c.XAM("XNotifyGetNext", {listener, 0, id_ptr, param_ptr}) == 1) {}
    c.GetRuntime().GetNotifier().BroadcastNotification(0x11, 5);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {listener, 1, 0, c.Quad(-50000000)}) == 0);
    CHECK(c.XAM("XNotifyGetNext", {listener, 0, id_ptr, param_ptr}) == 1);
    CHECK(c.Load32(id_ptr) == 0x11);
    CHECK(c.Load32(param_ptr) == 5);
    CHECK(c.Kernel("NtClose", {listener}) == 0);
  });
}

REX_TEST_CASE("notifications: mask filters", "[kernel][notify]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 listener = c.XAM("XamNotifyCreateListener", {0x1, 0});
    CHECKED_ELSE(listener != 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 id_ptr = c.Alloc(4);
    const u32 param_ptr = c.Alloc(4);
    while (c.XAM("XNotifyGetNext", {listener, 0, id_ptr, param_ptr}) == 1) {}
    c.GetRuntime().GetNotifier().BroadcastNotification(0x02000011, 9);
    CHECK(c.XAM("XNotifyGetNext", {listener, 0, id_ptr, param_ptr}) == 0);
    CHECK(c.Load32(id_ptr) == 0);
    c.GetRuntime().GetNotifier().BroadcastNotification(0x11, 5);
    CHECK(c.XAM("XNotifyGetNext", {listener, 0, id_ptr, param_ptr}) == 1);
    CHECK(c.Load32(id_ptr) == 0x11);
    CHECK(c.Kernel("NtClose", {listener}) == 0);
  });
}
