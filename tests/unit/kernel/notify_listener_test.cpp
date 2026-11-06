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

#include <rex/kernel/object/notification_broadcaster.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/title_runtime.h>

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
    c.GetRuntime().GetNotificationBroadcaster().BroadcastNotification(0x11, 5);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {listener, 1, 0, c.AllocQuad(-50000000)}) == 0);
    CHECK(c.XAM("XNotifyGetNext", {listener, 0, id_ptr, param_ptr}) == 1);
    CHECK(c.Load32(id_ptr) == 0x11);
    CHECK(c.Load32(param_ptr) == 5);
    CHECK(c.Kernel("NtClose", {listener}) == 0);
  });
}

REX_TEST_CASE("notifications: a referenced listener dies with its dereference",
              "[kernel][notify]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 listener = c.XAM("XamNotifyCreateListener", {0x1, 0});
    CHECKED_ELSE(listener != 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 object_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("ObReferenceObjectByHandle", {listener, 0, object_ptr}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 body = c.Load32(object_ptr);
    CHECK(body != 0);
    auto& table = c.GetRuntime().GetObjectTable();
    CHECK(c.Kernel("NtClose", {listener}) == 0);
    CHECK(static_cast<bool>(table.GetByAddress(body)));
    CHECK(c.Kernel("ObDereferenceObject", {body}) == 0);
    CHECK_FALSE(static_cast<bool>(table.GetByAddress(body)));
  });
}

REX_TEST_CASE("notifications: closing the last handle destroys the listener", "[kernel][notify]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 gone = c.XAM("XamNotifyCreateListener", {~0ull, 0});
    const u32 kept = c.XAM("XamNotifyCreateListener", {~0ull, 0});
    CHECKED_ELSE((gone != 0 && kept != 0)) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 id_ptr = c.Alloc(4);
    const u32 param_ptr = c.Alloc(4);
    while (c.XAM("XNotifyGetNext", {kept, 0, id_ptr, param_ptr}) == 1) {}
    auto& table = c.GetRuntime().GetObjectTable();
    const u32 body = table.Get(gone).GetGuestAddress();
    REQUIRE(body != 0);
    CHECK(c.Kernel("NtClose", {gone}) == 0);
    CHECK_FALSE(static_cast<bool>(table.GetByAddress(body)));
    c.GetRuntime().GetNotificationBroadcaster().BroadcastNotification(0x11, 5);
    CHECK(c.XAM("XNotifyGetNext", {kept, 0, id_ptr, param_ptr}) == 1);
    CHECK(c.Load32(id_ptr) == 0x11);
    CHECK(c.Kernel("NtClose", {kept}) == 0);
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
    c.GetRuntime().GetNotificationBroadcaster().BroadcastNotification(0x02000011, 9);
    CHECK(c.XAM("XNotifyGetNext", {listener, 0, id_ptr, param_ptr}) == 0);
    CHECK(c.Load32(id_ptr) == 0);
    c.GetRuntime().GetNotificationBroadcaster().BroadcastNotification(0x11, 5);
    CHECK(c.XAM("XNotifyGetNext", {listener, 0, id_ptr, param_ptr}) == 1);
    CHECK(c.Load32(id_ptr) == 0x11);
    CHECK(c.Kernel("NtClose", {listener}) == 0);
  });
}
