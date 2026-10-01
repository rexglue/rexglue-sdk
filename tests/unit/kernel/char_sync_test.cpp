#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <future>
#include <memory>

#include <rex/runtime/function_dispatcher.h>
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

TEST_CASE("semaphores: create release wait and limit", "[char][semaphores]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 previous = c.Alloc(4);
    REQUIRE(c.Kernel("NtCreateSemaphore", {handle_ptr, 0, 1, 2}) == 0);
    const u32 handle = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == kTimeout);
    CHECK(c.Kernel("NtReleaseSemaphore", {handle, 1, previous}) == 0);
    CHECK(c.Load32(previous) == 0);
    CHECK(c.Kernel("NtReleaseSemaphore", {handle, 1, previous}) == 0);
    CHECK(c.Load32(previous) == 1);
    CHECK(c.Kernel("NtReleaseSemaphore", {handle, 1, previous}) ==
          X_STATUS_SEMAPHORE_LIMIT_EXCEEDED);
    CHECK(c.Kernel("NtReleaseSemaphore", {0xF8001230, 1, previous}) == kInvalidHandle);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

TEST_CASE("semaphores: guest KSEMAPHORE", "[char][semaphores][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 sem = c.Alloc(0x14);
    c.Kernel("KeInitializeSemaphore", {sem, 1, 3});
    CHECK(c.Kernel("KeWaitForSingleObject", {sem, 0, 0, 0, Poll(c)}) == 0);
    CHECK(c.Kernel("KeWaitForSingleObject", {sem, 0, 0, 0, Poll(c)}) == kTimeout);
    CHECK(c.Kernel("KeReleaseSemaphore", {sem, 0, 2, 0}) == 0);
    CHECK(c.Kernel("KeWaitForSingleObject", {sem, 0, 0, 0, Poll(c)}) == 0);
  });
}

TEST_CASE("mutants: recursive ownership and release", "[char][mutants]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    REQUIRE(c.Kernel("NtCreateMutant", {handle_ptr, 0, 1}) == 0);
    const u32 handle = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == 0);
    CHECK(c.Kernel("NtReleaseMutant", {handle, 0}) == 0);
    CHECK(c.Kernel("NtReleaseMutant", {handle, 0}) == 0);
    CHECK(c.Kernel("NtReleaseMutant", {handle, 0}) == X_STATUS_MUTANT_NOT_OWNED);
    CHECK(c.Kernel("NtReleaseMutant", {0xF8001230, 0}) == kInvalidHandle);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

TEST_CASE("mutants: another thread cannot take an owned mutant", "[char][mutants]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    REQUIRE(c.Kernel("NtCreateMutant", {handle_ptr, 0, 1}) == 0);
    const u32 handle = c.Load32(handle_ptr);
    auto other_result = std::make_shared<std::promise<u32>>();
    auto other_done = other_result->get_future();
    const u32 other = c.Spawn([handle, other_result](Caller& o) {
      other_result->set_value(o.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, o.Quad(-10000)}));
    });
    CHECK(other_done.get() == kTimeout);
    CHECK(c.Kernel("NtReleaseMutant", {handle, 0}) == 0);
    c.Kernel("NtClose", {other});
    c.Kernel("NtClose", {handle});
  });
}

TEST_CASE("timers: create set wait cancel", "[char][timers]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    REQUIRE(c.Kernel("NtCreateTimer", {handle_ptr, 0, 0}) == 0);
    const u32 handle = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == kTimeout);
    CHECK(c.Kernel("NtSetTimerEx", {handle, c.Quad(-100000), 0, 1, 0, 0, 0, 0}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, c.Quad(-5000000)}) == 0);
    CHECK(c.Kernel("NtCancelTimer", {handle, 0}) == 0);
    CHECK(c.Kernel("NtSetTimerEx", {handle, c.Quad(-100000), 0, 1, 0, 1, 0, 0}) ==
          X_STATUS_TIMER_RESUME_IGNORED);
    CHECK(c.Kernel("NtSetTimerEx", {0xF8001230, c.Quad(-100000), 0, 1, 0, 0, 0, 0}) ==
          kInvalidHandle);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

TEST_CASE("timers: completion routine runs as an APC on the setting thread",
          "[char][timers][apc]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    static std::atomic<u32> g_routine_calls;
    g_routine_calls = 0;
    auto* dispatcher = c.runtime().function_dispatcher();
    const u32 routine = dispatcher->AllocateThunk(
        +[](PPCContext& ctx, u8*) {
          ++g_routine_calls;
          ctx.r3.u64 = 0;
        },
        0);
    const u32 handle_ptr = c.Alloc(4);
    REQUIRE(c.Kernel("NtCreateTimer", {handle_ptr, 0, 0}) == 0);
    const u32 handle = c.Load32(handle_ptr);
    REQUIRE(c.Kernel("NtSetTimerEx", {handle, c.Quad(-100000), routine, 1, 0, 0, 0, 0}) == 0);
    const u32 event_ptr = c.Alloc(4);
    REQUIRE(c.Kernel("NtCreateEvent", {event_ptr, 0, 0, 0}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {c.Load32(event_ptr), 1, 1, c.Quad(-5000000)}) ==
          X_STATUS_USER_APC);
    CHECK(g_routine_calls == 1);
    c.Kernel("NtClose", {handle});
    c.Kernel("NtClose", {c.Load32(event_ptr)});
  });
}
