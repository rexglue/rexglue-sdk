#include <catch2/catch_test_macros.hpp>

#include <rex/runtime/xtypes.h>

#include "kernel_fixture.h"

using rex::X_STATUS;

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {
constexpr u32 kInvalidHandle = X_STATUS_INVALID_HANDLE;
constexpr u32 kTimeout = X_STATUS_TIMEOUT;
u32 Poll(Caller& c) {
  return c.Quad(0);
}
}  // namespace

TEST_CASE("events: manual reset create set wait clear", "[char][events]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    REQUIRE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 0}) == 0);
    const u32 handle = c.Load32(handle_ptr);
    REQUIRE(handle != 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == kTimeout);
    const u32 previous = c.Alloc(4);
    CHECK(c.Kernel("NtSetEvent", {handle, previous}) == 0);
    CHECK(c.Load32(previous) == 1);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == 0);
    CHECK(c.Kernel("NtClearEvent", {handle}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == kTimeout);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

TEST_CASE("events: auto reset is consumed by one wait", "[char][events]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    REQUIRE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 1, 1}) == 0);
    const u32 handle = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == kTimeout);
    CHECK(c.Kernel("NtSetEvent", {handle, 0}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == kTimeout);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

TEST_CASE("events: query reports type and state", "[char][events]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 out = c.Alloc(8);
    REQUIRE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 1}) == 0);
    const u32 manual = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtQueryEvent", {manual, out}) == 0);
    CHECK(c.Load32(out + 4) == 1);
    REQUIRE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 1, 0}) == 0);
    const u32 sync = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtQueryEvent", {sync, out}) == 0);
    CHECK(c.Load32(out + 4) == 0);
    CHECK(c.Kernel("NtQueryEvent", {0xF8001230, out}) == kInvalidHandle);
    c.Kernel("NtClose", {manual});
    c.Kernel("NtClose", {sync});
  });
}

TEST_CASE("events: pulse and invalid handles", "[char][events]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 previous = c.Alloc(4);
    REQUIRE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 0}) == 0);
    const u32 handle = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtPulseEvent", {handle, previous}) == 0);
    CHECK(c.Load32(previous) == 1);
    CHECK(c.Kernel("NtSetEvent", {0, 0}) == kInvalidHandle);
    CHECK(c.Kernel("NtSetEvent", {0xF8001230, 0}) == kInvalidHandle);
    CHECK(c.Kernel("NtClearEvent", {0xF8001230}) == kInvalidHandle);
    CHECK(c.Kernel("NtPulseEvent", {0xF8001230, 0}) == kInvalidHandle);
    c.Kernel("NtClose", {handle});
  });
}

TEST_CASE("events: guest KEVENT initialized and lazily created", "[char][events][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 initialized = c.Alloc(0x10);
    c.Kernel("KeInitializeEvent", {initialized, 1, 1});
    CHECK(c.Load32(initialized + 8) != 0);
    CHECK(c.Kernel("KeWaitForSingleObject", {initialized, 0, 0, 0, Poll(c)}) == 0);
    CHECK(c.Kernel("KeWaitForSingleObject", {initialized, 0, 0, 0, Poll(c)}) == kTimeout);
    CHECK(c.Kernel("KeSetEvent", {initialized, 0, 0}) == 1);
    CHECK(c.Kernel("KeWaitForSingleObject", {initialized, 0, 0, 0, Poll(c)}) == 0);
    CHECK(c.Kernel("KeResetEvent", {initialized}) == 1);

    const u32 inlined = c.Alloc(0x10);
    c.Store32(inlined, 0x01000000);
    c.Store32(inlined + 4, 1);
    CHECK(c.Kernel("KeWaitForSingleObject", {inlined, 0, 0, 0, Poll(c)}) == 0);
    CHECK(c.Load32(inlined + 12) != 0);
    CHECK(c.Kernel("NtSetEvent", {c.Load32(inlined + 12), 0}) == 0);
    CHECK(c.Kernel("KeWaitForSingleObject", {inlined, 0, 0, 0, Poll(c)}) == 0);
  });
}
