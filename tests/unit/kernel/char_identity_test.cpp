#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("notifications: listener receives a matching broadcast", "[char][notify]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 listener = c.XAM("XamNotifyCreateListener", {~0ull, 0});
    REQUIRE(listener != 0);
    const u32 id_ptr = c.Alloc(4);
    const u32 param_ptr = c.Alloc(4);
    while (c.XAM("XNotifyGetNext", {listener, 0, id_ptr, param_ptr}) == 1) {}
    c.runtime().notifier().BroadcastNotification(0x11, 5);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {listener, 1, 0, c.Quad(-50000000)}) == 0);
    CHECK(c.XAM("XNotifyGetNext", {listener, 0, id_ptr, param_ptr}) == 1);
    CHECK(c.Load32(id_ptr) == 0x11);
    CHECK(c.Load32(param_ptr) == 5);
    CHECK(c.Kernel("NtClose", {listener}) == 0);
  });
}

TEST_CASE("identity: the stash survives and survives repeated resolution", "[char][identity]") {
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

TEST_CASE("identity: ObReferenceObjectByHandle returns the same guest pointer for the same object",
          "[char][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 first = c.Alloc(4);
    const u32 second = c.Alloc(4);
    REQUIRE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 0}) == 0);
    const u32 handle = c.Load32(handle_ptr);
    REQUIRE(c.Kernel("ObReferenceObjectByHandle", {handle, 0, first}) == 0);
    REQUIRE(c.Kernel("ObReferenceObjectByHandle", {handle, 0, second}) == 0);
    CHECK(c.Load32(first) == c.Load32(second));
    c.Kernel("ObDereferenceObject", {c.Load32(first)});
    c.Kernel("ObDereferenceObject", {c.Load32(second)});
    c.Kernel("NtClose", {handle});
  });
}

TEST_CASE("identity: the current thread pseudo handle", "[char][identity]") {
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

TEST_CASE("notifications: mask filters", "[char][notify]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 listener = c.XAM("XamNotifyCreateListener", {0x1, 0});
    REQUIRE(listener != 0);
    const u32 id_ptr = c.Alloc(4);
    const u32 param_ptr = c.Alloc(4);
    while (c.XAM("XNotifyGetNext", {listener, 0, id_ptr, param_ptr}) == 1) {}
    c.runtime().notifier().BroadcastNotification(0x02000011, 9);
    CHECK(c.XAM("XNotifyGetNext", {listener, 0, id_ptr, param_ptr}) == 0);
    CHECK(c.Load32(id_ptr) == 0);
    c.runtime().notifier().BroadcastNotification(0x11, 5);
    CHECK(c.XAM("XNotifyGetNext", {listener, 0, id_ptr, param_ptr}) == 1);
    CHECK(c.Load32(id_ptr) == 0x11);
    CHECK(c.Kernel("NtClose", {listener}) == 0);
  });
}
