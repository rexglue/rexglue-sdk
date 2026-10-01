#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/xtypes.h>

#include "kernel_fixture.h"

using rex::X_STATUS;

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {}  // namespace

TEST_CASE("threads: create suspended resume and wait for exit", "[char][threads]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto ran = std::make_shared<std::atomic<u32>>(0);
    const u32 handle = c.Spawn([ran](Caller&) { ran->store(1); }, 1);
    CHECK(ran->load() == 0);
    const u32 count = c.Alloc(4);
    CHECK(c.Kernel("NtResumeThread", {handle, count}) == 0);
    CHECK(c.Load32(count) == 1);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, c.Quad(-50000000)}) == 0);
    CHECK(ran->load() == 1);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

TEST_CASE("threads: suspend and resume counts", "[char][threads]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle = c.Spawn([](Caller&) {}, 1);
    const u32 count = c.Alloc(4);
    CHECK(c.Kernel("NtSuspendThread", {handle, count}) == 0);
    CHECK(c.Load32(count) == 1);
    CHECK(c.Kernel("NtResumeThread", {handle, count}) == 0);
    CHECK(c.Load32(count) == 2);
    CHECK(c.Kernel("NtResumeThread", {handle, count}) == 0);
    CHECK(c.Load32(count) == 1);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, c.Quad(-50000000)}) == 0);
    c.Kernel("NtClose", {handle});
  });
}

TEST_CASE("threads: a handle outlives the thread", "[char][threads]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle = c.Spawn([](Caller&) {});
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, c.Quad(-50000000)}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, c.Quad(0)}) == 0);
    const u32 copy_ptr = c.Alloc(4);
    CHECK(c.Kernel("NtDuplicateObject", {handle, copy_ptr, 0}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {c.Load32(copy_ptr), 1, 0, c.Quad(0)}) == 0);
    CHECK(c.Kernel("NtClose", {c.Load32(copy_ptr)}) == 0);
  });
}

TEST_CASE("threads: ObLookupThreadByThreadId finds a live thread and not a dead id",
          "[char][threads]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 id_ptr = c.Alloc(4);
    const u32 handle_ptr = c.Alloc(4);
    auto release = std::make_shared<std::promise<void>>();
    auto gate = release->get_future().share();
    REQUIRE(c.Kernel("ExCreateThread",
                     {handle_ptr, 0x10000, id_ptr, 0, Caller::start_thunk,
                      Caller::PutBodyForTest([gate](Caller&) { gate.wait(); }), 0}) == 0);
    const u32 id = c.Load32(id_ptr);
    const u32 object_ptr = c.Alloc(4);
    CHECK(c.Kernel("ObLookupThreadByThreadId", {id, object_ptr}) == 0);
    CHECK(c.Load32(object_ptr) != 0);
    CHECK(c.Kernel("ObDereferenceObject", {c.Load32(object_ptr)}) == 0);
    release->set_value();
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {c.Load32(handle_ptr), 1, 0, c.Quad(-50000000)}) ==
          0);
    c.Kernel("NtClose", {c.Load32(handle_ptr)});
    CHECK(c.Kernel("ObLookupThreadByThreadId", {0x7FFFFFF0, object_ptr}) == X_STATUS_NOT_FOUND);
  });
}

TEST_CASE("threads: ExTerminateThread ends the thread and signals its handle", "[char][threads]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle = c.Spawn([](Caller& o) { o.Kernel("ExTerminateThread", {7}); });
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, c.Quad(-50000000)}) == 0);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

TEST_CASE("threads: a queued user APC runs during an alertable wait", "[char][threads][apc]") {
  static std::atomic<u32> g_apc_calls;
  g_apc_calls = 0;
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto* dispatcher = c.runtime().function_dispatcher();
    const u32 routine = dispatcher->AllocateThunk(
        +[](PPCContext& ctx, u8*) {
          ++g_apc_calls;
          ctx.r3.u64 = 0;
        },
        0);
    auto wait_result = std::make_shared<std::promise<u32>>();
    auto done = wait_result->get_future();
    auto waiting = std::make_shared<std::atomic<bool>>(false);
    const u32 thread = c.Spawn([wait_result, waiting](Caller& o) {
      const u32 event_ptr = o.Alloc(4);
      o.Kernel("NtCreateEvent", {event_ptr, 0, 0, 0});
      waiting->store(true);
      wait_result->set_value(
          o.Kernel("NtWaitForSingleObjectEx", {o.Load32(event_ptr), 1, 1, o.Quad(-100000000)}));
    });
    while (!waiting->load()) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    c.Kernel("NtQueueApcThread", {thread, routine, 5, 6, 7});
    CHECK(done.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    CHECK(done.get() == X_STATUS_USER_APC);
    CHECK(g_apc_calls == 1);
    c.Kernel("NtClose", {thread});
  });
}
