/**
 * @file        tests/unit/kernel/notify_test.cpp
 * @brief       Title terminate notifications and thread notify routines
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <rex/kernel/title_runtime.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/object/kernel_thread.h>

#include "kernel_fixture.h"
#include "kernel/title_state.h"

using rex::test::Caller;
using rex::test::KernelFixture;

namespace {

std::mutex g_mutex;
std::vector<u32> g_registrations;
std::vector<std::pair<u32, u32>> g_thread_calls;
std::atomic<u32> g_spawned_id{0};

void TerminateRoutine(PPCContext& ctx, u8*) {
  std::lock_guard lock(g_mutex);
  g_registrations.push_back(ctx.r3.u32);
  ctx.r3.u64 = 0;
}

std::atomic<bool> g_slow_routine_done{false};

void SlowTerminateRoutine(PPCContext& ctx, u8*) {
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  g_slow_routine_done = true;
  ctx.r3.u64 = 0;
}

void ThreadNotifyRoutine(PPCContext& ctx, u8*) {
  const u32 id = rex::kernel::KernelThread::GetCallingThread()->GetThreadID();
  std::lock_guard lock(g_mutex);
  g_thread_calls.emplace_back(id, ctx.r3.u32);
  ctx.r3.u64 = 0;
}

rex::kernel::TitleRuntime* g_runtime = nullptr;
std::atomic<u32> g_exit_event{0};
std::atomic<bool> g_in_exit_routine{false};

void StuckTerminateRoutine(PPCContext& ctx, u8* base) {
  Caller caller(ctx, base, *g_runtime);
  caller.Kernel("NtWaitForSingleObjectEx", {g_exit_event.load(), 1, 0, 0});
  ctx.r3.u64 = 0;
}

void WaitingExitNotifyRoutine(PPCContext& ctx, u8* base) {
  if (ctx.r3.u32 == 0 && g_exit_event) {
    g_in_exit_routine = true;
    Caller caller(ctx, base, *g_runtime);
    caller.Kernel("NtWaitForSingleObjectEx", {g_exit_event.load(), 1, 0, 0});
  }
  ctx.r3.u64 = 0;
}

void Reset() {
  std::lock_guard lock(g_mutex);
  g_registrations.clear();
  g_thread_calls.clear();
  g_spawned_id = 0;
}

u32 Registration(Caller& c, u32 routine, u32 priority) {
  const u32 registration = c.Alloc(0x10);
  c.Store32(registration, routine);
  c.Store32(registration + 4, priority);
  c.Kernel("ExRegisterTitleTerminateNotification", {registration, 1});
  return registration;
}

u32 ThreadNotification(Caller& c) {
  const u32 routine =
      StateOf(c.GetRuntime()).GetFunctionDispatcher()->AllocateThunk(&ThreadNotifyRoutine, 0);
  const u32 notification = c.Alloc(0xC);
  c.Store32(notification + 8, routine);
  c.XAM("XRegisterThreadNotifyRoutine", {notification, 1});
  return notification;
}

std::vector<u32> CallsFor(u32 thread_id) {
  std::lock_guard lock(g_mutex);
  std::vector<u32> calls;
  for (const auto& [id, create] : g_thread_calls) {
    if (id == thread_id) {
      calls.push_back(create);
    }
  }
  return calls;
}

}  // namespace

REX_TEST_CASE("terminate notifications run by priority with their registration",
              "[kernel][notify]") {
  Reset();
  std::vector<u32> expected;
  KernelFixture fixture;
  fixture.RunOnGuestThread([&expected](Caller& c) {
    const u32 routine =
        StateOf(c.GetRuntime()).GetFunctionDispatcher()->AllocateThunk(&TerminateRoutine, 0);
    const u32 low = Registration(c, routine, 1);
    const u32 high = Registration(c, routine, 5);
    const u32 middle_first = Registration(c, routine, 3);
    const u32 middle_second = Registration(c, routine, 3);
    const u32 removed = Registration(c, routine, 9);
    c.Kernel("ExRegisterTitleTerminateNotification", {removed, 0});
    expected = {high, middle_first, middle_second, low};
  });
  StateOf(fixture.GetRuntime()).TerminateTitle();
  std::lock_guard lock(g_mutex);
  CHECK(g_registrations == expected);
}

REX_TEST_CASE("a terminate notification blocked in host code is joined after the terminate timeout",
              "[kernel][notify]") {
  Reset();
  g_slow_routine_done = false;
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 routine =
        StateOf(c.GetRuntime()).GetFunctionDispatcher()->AllocateThunk(&SlowTerminateRoutine, 0);
    Registration(c, routine, 1);
  });
  const auto start = std::chrono::steady_clock::now();
  StateOf(fixture.GetRuntime()).TerminateTitle();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  CHECK(elapsed >= 1900);
  CHECK(g_slow_routine_done.load());
}

REX_TEST_CASE("thread notify runs on create and on return from the start routine",
              "[kernel][notify]") {
  Reset();
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    ThreadNotification(c);
    const u32 thread = c.Spawn([](Caller&) {
      g_spawned_id = rex::kernel::KernelThread::GetCallingThread()->GetThreadID();
    });
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.AllocQuad(-50000000)}) == 0);
    c.Kernel("NtClose", {thread});
  });
  CHECK(CallsFor(g_spawned_id) == std::vector<u32>{1, 0});
}

REX_TEST_CASE("thread notify runs on ExTerminateThread", "[kernel][notify]") {
  Reset();
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    ThreadNotification(c);
    const u32 thread = c.Spawn([](Caller& o) {
      g_spawned_id = rex::kernel::KernelThread::GetCallingThread()->GetThreadID();
      o.Kernel("ExTerminateThread", {7});
    });
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.AllocQuad(-50000000)}) == 0);
    c.Kernel("NtClose", {thread});
  });
  CHECK(CallsFor(g_spawned_id) == std::vector<u32>{1, 0});
}

REX_TEST_CASE("thread notify does not run for a thread stopped by title termination",
              "[kernel][notify]") {
  Reset();
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    ThreadNotification(c);
    const u32 event_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {event_ptr, 0, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 event = c.Load32(event_ptr);
    c.Spawn([event](Caller& o) {
      g_spawned_id = rex::kernel::KernelThread::GetCallingThread()->GetThreadID();
      o.Kernel("NtWaitForSingleObjectEx", {event, 1, 0, 0});
    });
    for (int i = 0; i < 200 && g_spawned_id == 0; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  StateOf(fixture.GetRuntime()).TerminateTitle();
  CHECK(CallsFor(g_spawned_id) == std::vector<u32>{1});
}

REX_TEST_CASE("a removed thread notification is not called", "[kernel][notify]") {
  Reset();
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 notification = ThreadNotification(c);
    c.XAM("XRegisterThreadNotifyRoutine", {notification, 0});
    const u32 thread = c.Spawn([](Caller&) {
      g_spawned_id = rex::kernel::KernelThread::GetCallingThread()->GetThreadID();
    });
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.AllocQuad(-50000000)}) == 0);
    c.Kernel("NtClose", {thread});
  });
  CHECK(CallsFor(g_spawned_id).empty());
}

REX_TEST_CASE("a terminate notification that never returns ends exit after the terminate timeout",
              "[kernel][notify]") {
  Reset();
  KernelFixture fixture;
  g_runtime = &fixture.GetRuntime();
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {event_ptr, 0, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    g_exit_event = c.Load32(event_ptr);
    const u32 routine =
        StateOf(c.GetRuntime()).GetFunctionDispatcher()->AllocateThunk(&StuckTerminateRoutine, 0);
    Registration(c, routine, 1);
  });
  const auto start = std::chrono::steady_clock::now();
  StateOf(fixture.GetRuntime()).TerminateTitle();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  g_exit_event = 0;
  CHECK(elapsed >= 1900);
}

REX_TEST_CASE("title termination during an exit notify routine lets the thread exit",
              "[kernel][notify]") {
  Reset();
  g_in_exit_routine = false;
  KernelFixture fixture;
  g_runtime = &fixture.GetRuntime();
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {event_ptr, 0, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    g_exit_event = c.Load32(event_ptr);
    const u32 routine = StateOf(c.GetRuntime())
                            .GetFunctionDispatcher()
                            ->AllocateThunk(&WaitingExitNotifyRoutine, 0);
    const u32 notification = c.Alloc(0xC);
    c.Store32(notification + 8, routine);
    c.XAM("XRegisterThreadNotifyRoutine", {notification, 1});
    c.Spawn([](Caller&) {});
    for (int i = 0; i < 200 && !g_in_exit_routine; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  REQUIRE(g_in_exit_routine);
  const auto start = std::chrono::steady_clock::now();
  StateOf(fixture.GetRuntime()).TerminateTitle();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  g_exit_event = 0;
  CHECK(elapsed < 1500);
}
