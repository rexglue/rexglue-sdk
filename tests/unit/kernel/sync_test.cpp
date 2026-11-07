/**
 * @file        tests/unit/kernel/sync_test.cpp
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
#include <future>
#include <memory>
#include <thread>

#include <rex/kernel/types/thread.h>
#include <rex/kernel/xboxkrnl.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/platform/sync.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/object/object_table.h>

#include "kernel_fixture.h"
#include "kernel/title_state.h"
#include "kernel/kernel_fixture.h"
#include "elapsed.h"

using rex::kernel::X_STATUS;

using rex::test::Caller;
using rex::test::KernelFixture;
using rex::test::SystemTime;

namespace {
constexpr u32 kInvalidHandle = rex::kernel::X_STATUS_INVALID_HANDLE;
constexpr u32 kTimeout = rex::kernel::X_STATUS_TIMEOUT;
u32 Poll(Caller& c) {
  return c.AllocQuad(0);
}
}  // namespace

REX_TEST_CASE("semaphores: create release wait and limit", "[kernel][semaphores]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 previous = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateSemaphore", {handle_ptr, 0, 1, 2}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == kTimeout);
    CHECK(c.Kernel("NtReleaseSemaphore", {handle, 1, previous}) == 0);
    CHECK(c.Load32(previous) == 0);
    CHECK(c.Kernel("NtReleaseSemaphore", {handle, 1, previous}) == 0);
    CHECK(c.Load32(previous) == 1);
    CHECK(c.Kernel("NtReleaseSemaphore", {handle, 1, previous}) ==
          rex::kernel::X_STATUS_SEMAPHORE_LIMIT_EXCEEDED);
    CHECK(c.Kernel("NtReleaseSemaphore", {0xF8001230, 1, previous}) == kInvalidHandle);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("semaphores: guest KSEMAPHORE", "[kernel][semaphores][identity]") {
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

REX_TEST_CASE("mutants: recursive ownership and release", "[kernel][mutants]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateMutant", {handle_ptr, 0, 1}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == 0);
    CHECK(c.Kernel("NtReleaseMutant", {handle, 0}) == 0);
    CHECK(c.Kernel("NtReleaseMutant", {handle, 0}) == 0);
    CHECK(c.Kernel("NtReleaseMutant", {handle, 0}) == rex::kernel::X_STATUS_MUTANT_NOT_OWNED);
    CHECK(c.Kernel("NtReleaseMutant", {0xF8001230, 0}) == kInvalidHandle);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("mutants: another thread cannot take an owned mutant", "[kernel][mutants]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateMutant", {handle_ptr, 0, 1}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    auto other_result = std::make_shared<std::promise<u32>>();
    auto other_done = other_result->get_future();
    const u32 other = c.Spawn([handle, other_result](Caller& o) {
      other_result->set_value(
          o.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, o.AllocQuad(-10000)}));
    });
    CHECK(other_done.get() == kTimeout);
    CHECK(c.Kernel("NtReleaseMutant", {handle, 0}) == 0);
    c.Kernel("NtClose", {other});
    c.Kernel("NtClose", {handle});
  });
}

REX_TEST_CASE("timers: create set wait cancel", "[kernel][timers]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateTimer", {handle_ptr, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == kTimeout);
    CHECK(c.Kernel("NtSetTimerEx", {handle, c.AllocQuad(-100000), 0, 1, 0, 0, 0, 0}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, c.AllocQuad(-5000000)}) == 0);
    CHECK(c.Kernel("NtCancelTimer", {handle, 0}) == 0);
    CHECK(c.Kernel("NtSetTimerEx", {handle, c.AllocQuad(-100000), 0, 1, 0, 1, 0, 0}) ==
          rex::kernel::X_STATUS_TIMER_RESUME_IGNORED);
    CHECK(c.Kernel("NtSetTimerEx", {0xF8001230, c.AllocQuad(-100000), 0, 1, 0, 0, 0, 0}) ==
          kInvalidHandle);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("timers: completion routine runs as an APC on the setting thread",
              "[kernel][timers][apc]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    static std::atomic<u32> g_routine_calls;
    g_routine_calls = 0;
    auto* dispatcher = StateOf(c.GetRuntime()).GetFunctionDispatcher();
    const u32 routine = dispatcher->AllocateThunk(
        +[](PPCContext& ctx, u8*) {
          ++g_routine_calls;
          ctx.r3.u64 = 0;
        },
        0);
    const u32 handle_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateTimer", {handle_ptr, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    CHECKED_ELSE(c.Kernel("NtSetTimerEx", {handle, c.AllocQuad(-100000), routine, 1, 0, 0, 0, 0}) ==
                 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 event_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {event_ptr, 0, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {c.Load32(event_ptr), 1, 1, c.AllocQuad(-5000000)}) ==
          rex::kernel::X_STATUS_USER_APC);
    CHECK(g_routine_calls == 1);
    c.Kernel("NtClose", {handle});
    c.Kernel("NtClose", {c.Load32(event_ptr)});
  });
}

namespace {

u32 WaitObject(Caller& c, u32 object, i64 timeout) {
  return c.Kernel("KeWaitForSingleObject", {object, 0, 0, 0, c.AllocQuad(timeout)});
}

u32 WaitAll(Caller& c, std::initializer_list<u32> objects, i64 timeout) {
  const u32 array = c.Alloc(static_cast<u32>(objects.size() * 4));
  u32 offset = 0;
  for (const u32 object : objects) {
    c.Store32(array + offset, object);
    offset += 4;
  }
  return c.Kernel("KeWaitForMultipleObjects",
                  {static_cast<u32>(objects.size()), array, 0, 0, 1, 0, c.AllocQuad(timeout), 0});
}

}  // namespace

REX_TEST_CASE("mutants: a Ke mutant initialized with an owner is owned by the caller",
              "[kernel][mutants]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 mutant = c.Alloc(0x20);
    c.Kernel("KeInitializeMutant", {mutant, 1});
    CHECK(c.Load32(mutant + 4) == 0);
    CHECK(c.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == 0);
    CHECK(c.Load32(mutant + 4) == 1);
  });
}

REX_TEST_CASE("mutants: a Ke mutant counts recursive acquisitions", "[kernel][mutants]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 mutant = c.Alloc(0x20);
    c.Kernel("KeInitializeMutant", {mutant, 0});
    CHECK(WaitObject(c, mutant, -10000000) == 0);
    CHECK(WaitObject(c, mutant, -10000000) == 0);
    CHECK(c.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == 0xFFFFFFFF);
    CHECK(c.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == 0);
  });
}

REX_TEST_CASE("mutants: a Ke mutant release by a thread that does not own it fails",
              "[kernel][mutants]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 mutant = c.Alloc(0x20);
    c.Kernel("KeInitializeMutant", {mutant, 1});
    const u32 thread = c.Spawn([mutant](Caller& o) {
      CHECK(o.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) ==
            rex::kernel::X_STATUS_MUTANT_NOT_OWNED);
    });
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.AllocQuad(-50000000)}) == 0);
    c.Kernel("NtClose", {thread});
    CHECK(c.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == 0);
  });
}

REX_TEST_CASE("mutants: a wait all takes the Ke mutant only when every object is signaled",
              "[kernel][mutants]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 mutant = c.Alloc(0x20);
    c.Kernel("KeInitializeMutant", {mutant, 0});
    const u32 event = c.Alloc(0x10);
    c.Kernel("KeInitializeEvent", {event, 0, 0});
    const u32 thread = c.Spawn([mutant, event](Caller& o) {
      CHECK(WaitAll(o, {mutant, event}, -500000) == kTimeout);
      CHECK(o.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) ==
            rex::kernel::X_STATUS_MUTANT_NOT_OWNED);
      o.Kernel("KeSetEvent", {event, 0, 0});
      CHECK(WaitAll(o, {mutant, event}, -10000000) == 0);
      CHECK(o.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == 0);
    });
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.AllocQuad(-50000000)}) == 0);
    c.Kernel("NtClose", {thread});
  });
}

REX_TEST_CASE("mutants: a Ke mutant release hands ownership to 1 waiter", "[kernel][mutants]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 mutant = c.Alloc(0x20);
    c.Kernel("KeInitializeMutant", {mutant, 1});
    auto acquired = std::make_shared<std::atomic<int>>(0);
    auto waiter = [mutant, acquired](Caller& o) {
      CHECK(WaitObject(o, mutant, -50000000) == 0);
      ++*acquired;
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      CHECK(o.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == 0);
    };
    const u32 first = c.Spawn(waiter);
    const u32 second = c.Spawn(waiter);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(c.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(acquired->load() == 1);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {first, 1, 0, c.AllocQuad(-50000000)}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {second, 1, 0, c.AllocQuad(-50000000)}) == 0);
    CHECK(acquired->load() == 2);
    c.Kernel("NtClose", {first});
    c.Kernel("NtClose", {second});
  });
}

REX_TEST_CASE("events: KeInitializeEvent on an initialized event reuses its object",
              "[kernel][events]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event = c.Alloc(0x10);
    c.Kernel("KeInitializeEvent", {event, 0, 1});
    auto& table = c.GetRuntime().GetObjectTable();
    const auto registered = table.GetByAddress(event);
    REQUIRE(registered);
    c.Kernel("KeInitializeEvent", {event, 0, 0});
    const auto again = table.GetByAddress(event);
    REQUIRE(again);
    CHECK(&again.GetKernelObject() == &registered.GetKernelObject());
    CHECK(c.Load32(event + 8) == event + 8);
    CHECK(c.Load32(event + 0xC) == event + 8);
    CHECK(WaitObject(c, event, 0) == kTimeout);
  });
}

namespace {

std::atomic<int> g_timer_dpc_runs{0};
std::atomic<u64> g_timer_dpc_time{0};

void TimerDPC(PPCContext& ctx, u8*) {
  g_timer_dpc_time = (static_cast<u64>(ctx.r6.u32) << 32) | ctx.r5.u32;
  ++g_timer_dpc_runs;
  ctx.r3.u64 = 0;
}

u32 KeTimer(Caller& c, u32 type) {
  const u32 timer = c.Alloc(0x28);
  c.Kernel("KeInitializeTimerEx", {timer, type, 0});
  return timer;
}

u32 WaitTimer(Caller& c, u32 timer, i64 timeout) {
  return c.Kernel("KeWaitForSingleObject", {timer, 0, 0, 0, c.AllocQuad(timeout)});
}

}  // namespace

REX_TEST_CASE("timers: a notification Ke timer stays signaled", "[kernel][timers]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 timer = KeTimer(c, 0);
    CHECK(c.Load8(timer) == 8);
    CHECK(WaitTimer(c, timer, 0) == kTimeout);
    CHECK(c.Kernel("KeSetTimerEx", {timer, static_cast<u64>(-100000), 0, 0}) == 0);
    CHECK(WaitTimer(c, timer, -10000000) == 0);
    CHECK(WaitTimer(c, timer, 0) == 0);
    CHECK(c.Load8(timer + 3) == 0);
  });
}

REX_TEST_CASE("timers: a synchronization Ke timer resets on a satisfied wait", "[kernel][timers]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 timer = KeTimer(c, 1);
    CHECK(c.Load8(timer) == 9);
    CHECK(c.Kernel("KeSetTimer", {timer, static_cast<u64>(-100000), 0}) == 0);
    CHECK(WaitTimer(c, timer, -10000000) == 0);
    CHECK(WaitTimer(c, timer, 0) == kTimeout);
  });
}

REX_TEST_CASE("timers: KeSetTimerEx replaces a set timer and KeCancelTimer reports it",
              "[kernel][timers]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 timer = KeTimer(c, 0);
    CHECK(c.Kernel("KeSetTimerEx", {timer, static_cast<u64>(-100000000), 0, 0}) == 0);
    const u32 dpc = c.Alloc(0x20);
    CHECK(c.Kernel("KeSetTimerEx", {timer, static_cast<u64>(-200000000), 40, dpc}) == 1);
    CHECK(c.Load32(timer + 0x14) == static_cast<u32>(static_cast<u64>(-200000000)));
    CHECK(c.Load32(timer + 0x20) == dpc);
    CHECK(c.Load32(timer + 0x24) == 40);
    CHECK(c.Kernel("KeCancelTimer", {timer}) == 1);
    CHECK(c.Kernel("KeCancelTimer", {timer}) == 0);
    CHECK(WaitTimer(c, timer, -500000) == kTimeout);
  });
}

REX_TEST_CASE("timers: a Ke timer queues its DPC with the expiry time", "[kernel][timers][dpc]") {
  g_timer_dpc_runs = 0;
  g_timer_dpc_time = 0;
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 routine =
        StateOf(c.GetRuntime()).GetFunctionDispatcher()->AllocateThunk(&TimerDPC, 0);
    const u32 dpc = c.Alloc(0x20);
    c.Kernel("KeInitializeDpc", {dpc, routine, 0});
    const u32 timer = KeTimer(c, 0);
    const u64 before = SystemTime(c);
    CHECK(c.Kernel("KeSetTimerEx", {timer, static_cast<u64>(-100000), 0, dpc}) == 0);
    CHECK(WaitTimer(c, timer, -10000000) == 0);
    for (int i = 0; i < 200 && g_timer_dpc_runs == 0; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(g_timer_dpc_runs == 1);
    CHECK(g_timer_dpc_time >= before);
    CHECK(g_timer_dpc_time <= SystemTime(c));
  });
}

REX_TEST_CASE("timers: a periodic Ke timer rearms until canceled", "[kernel][timers][dpc]") {
  g_timer_dpc_runs = 0;
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 routine =
        StateOf(c.GetRuntime()).GetFunctionDispatcher()->AllocateThunk(&TimerDPC, 0);
    const u32 dpc = c.Alloc(0x20);
    c.Kernel("KeInitializeDpc", {dpc, routine, 0});
    const u32 timer = KeTimer(c, 1);
    CHECK(c.Kernel("KeSetTimerEx", {timer, static_cast<u64>(-100000), 20, dpc}) == 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (g_timer_dpc_runs < 3 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(c.Kernel("KeCancelTimer", {timer}) == 1);
    CHECK(g_timer_dpc_runs >= 3);
  });
}

REX_TEST_CASE("interrupts: connect, double connect and disconnect", "[kernel][interrupts]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 interrupt = c.Alloc(0x14);
    c.Kernel("KeInitializeInterrupt", {interrupt, 0x82000000, 0x1234, 0x1C, 0x1D, 1, 2});
    CHECK(c.Load32(interrupt) == 0x82000000);
    CHECK(c.Load32(interrupt + 4) == 0x1234);
    CHECK(c.Load32(interrupt + 0xC) == 0);
    CHECK(c.Load8(interrupt + 0x10) == 0x1C);
    CHECK(c.Load8(interrupt + 0x11) == 0x1D);
    CHECK(c.Load8(interrupt + 0x12) == 0x02);
    CHECK(c.Load8(interrupt + 0x13) == 2);
    CHECK(c.Kernel("KeConnectInterrupt", {interrupt}) == 1);
    CHECK(c.Load8(interrupt + 0x12) == 0x03);
    CHECK(c.Kernel("KeConnectInterrupt", {interrupt}) == 0);
    CHECK(c.Kernel("KeDisconnectInterrupt", {interrupt}) == 1);
    CHECK(c.Kernel("KeDisconnectInterrupt", {interrupt}) == 0);
    CHECK(c.Load8(interrupt + 0x12) == 0x02);
  });
}

REX_TEST_CASE("interrupts: KeQueryInterruptTime grows in 100 ns units", "[kernel][interrupts]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    c.Kernel("KeQueryInterruptTime");
    const u64 first = c.GetContext().r3.u64;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    c.Kernel("KeQueryInterruptTime");
    const u64 second = c.GetContext().r3.u64;
    CHECK(second - first >= 300000);
    CHECK(second - first < 50000000);
  });
}

REX_TEST_CASE("timers: a Ke timer with an absolute due time of 0 expires at once",
              "[kernel][timers]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 timer = KeTimer(c, 0);
    CHECK(c.Kernel("KeSetTimer", {timer, 0, 0}) == 0);
    CHECK(WaitTimer(c, timer, -10000000) == 0);
  });
}

REX_TEST_CASE("mutants: a Ke mutant abandoned by its owner is released by the next owner",
              "[kernel][mutants]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 mutant = c.Alloc(0x20);
    c.Kernel("KeInitializeMutant", {mutant, 0});
    const u32 thread =
        c.Spawn([mutant](Caller& o) { CHECK(WaitObject(o, mutant, -10000000) == 0); });
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.AllocQuad(-50000000)}) == 0);
    c.Kernel("NtClose", {thread});
    CHECK(WaitObject(c, mutant, -10000000) == rex::kernel::X_STATUS_ABANDONED_WAIT_0);
    CHECK(c.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == 0);
  });
}

REX_TEST_CASE(
    "mutants: a Ke mutant abandoned by a release from its owner is free and reports it once",
    "[kernel][mutants]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 mutant = c.Alloc(0x20);
    c.Kernel("KeInitializeMutant", {mutant, 0});
    CHECK(WaitObject(c, mutant, -10000000) == 0);
    CHECK(WaitObject(c, mutant, -10000000) == 0);
    CHECK(c.Kernel("KeReleaseMutant", {mutant, 0, 1, 0}) == 0xFFFFFFFF);
    CHECK(c.Load32(mutant + 4) == 1);
    CHECK(c.Load32(mutant + 0x18) == 0);
    CHECK(c.Load8(mutant + 0x1C) == 1);
    const u32 thread = c.Spawn([mutant](Caller& o) {
      CHECK(WaitObject(o, mutant, -10000000) == rex::kernel::X_STATUS_ABANDONED_WAIT_0);
      CHECK(o.Load8(mutant + 0x1C) == 0);
      CHECK(o.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == 0);
      CHECK(WaitObject(o, mutant, -10000000) == 0);
      CHECK(o.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == 0);
    });
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.AllocQuad(-50000000)}) == 0);
    c.Kernel("NtClose", {thread});
  });
}

REX_TEST_CASE("mutants: a Ke mutant abandon by a thread that does not own it fails",
              "[kernel][mutants]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 mutant = c.Alloc(0x20);
    c.Kernel("KeInitializeMutant", {mutant, 1});
    const u32 thread = c.Spawn([mutant](Caller& o) {
      CHECK(o.Kernel("KeReleaseMutant", {mutant, 0, 1, 0}) ==
            rex::kernel::X_STATUS_MUTANT_NOT_OWNED);
    });
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.AllocQuad(-50000000)}) == 0);
    c.Kernel("NtClose", {thread});
    CHECK(c.Load32(mutant + 4) == 0);
    CHECK(c.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == 0);
  });
}

REX_TEST_CASE("timers: a timer type other than 0 or 1 fails and creates no handle",
              "[kernel][timers]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    c.Store32(handle_ptr, 0x1234);
    CHECK(c.Kernel("NtCreateTimer", {handle_ptr, 0, 2}) == rex::kernel::X_STATUS_INVALID_PARAMETER);
    CHECK(c.Kernel("NtCreateTimer", {handle_ptr, 0, 0xFFFFFFFF}) ==
          rex::kernel::X_STATUS_INVALID_PARAMETER);
    CHECK(c.Load32(handle_ptr) == 0x1234);
    CHECK(c.Kernel("NtCreateTimer", {handle_ptr, 0, 1}) == 0);
    CHECK(c.Kernel("NtClose", {c.Load32(handle_ptr)}) == 0);
  });
}

REX_TEST_CASE("A title initialized semaphore with a limit above the int range works",
              "[kernel][semaphore]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 semaphore = c.Alloc(0x14);
    c.Kernel("KeInitializeSemaphore", {semaphore, 0, 0x80000000});
    CHECK(c.Kernel("KeReleaseSemaphore", {semaphore, 0, 1, 0}) == 0);
    CHECK(c.Kernel("KeReleaseSemaphore", {semaphore, 0, 1, 0}) == 1);
  });
}

using rex::u32;

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
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.AllocQuad(-50000000)}) == 0);
    c.Kernel("NtClose", {thread});
    CHECK(other->load() == 0);

    for (int i = 0; i < 3; ++i) {
      c.Kernel("RtlLeaveCriticalSection", {section});
    }
    CHECK(c.Kernel("RtlTryEnterCriticalSection", {section}) == 1);
    c.Kernel("RtlLeaveCriticalSection", {section});
  });
}

namespace wait_tests {

using rex::kernel::X_STATUS;

using rex::test::Caller;
using rex::test::KernelFixture;
using rex::test::SystemTime;

namespace {

using rex::test::MillisSince;

constexpr u32 kTimeout = rex::kernel::X_STATUS_TIMEOUT;
constexpr i64 kTicksPerMs = 10000;

u32 Event(Caller& c, u32 initial_state) {
  const u32 handle = c.Alloc(4);
  CHECKED_ELSE(c.Kernel("NtCreateEvent", {handle, 0, 0, initial_state}) == 0) {
    FAIL_CHECK("the check above failed");
    return 0;
  }
  return c.Load32(handle);
}

}  // namespace

REX_TEST_CASE("waits: an absolute deadline in the past times out at once", "[kernel][waits]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event = Event(c, 0);
    const u32 deadline = c.AllocQuad(static_cast<i64>(SystemTime(c)) - 1000 * kTicksPerMs);
    const auto start = std::chrono::steady_clock::now();
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {event, 1, 0, deadline}) == kTimeout);
    CHECK(MillisSince(start) < 500);
    c.Kernel("NtClose", {event});
  });
}

REX_TEST_CASE("waits: an absolute deadline of now times out at once", "[kernel][waits]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event = Event(c, 0);
    const u32 deadline = c.AllocQuad(static_cast<i64>(SystemTime(c)));
    const auto start = std::chrono::steady_clock::now();
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {event, 1, 0, deadline}) == kTimeout);
    CHECK(MillisSince(start) < 500);
    c.Kernel("NtClose", {event});
  });
}

REX_TEST_CASE("waits: an absolute deadline in the future waits until it", "[kernel][waits]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event = Event(c, 0);
    const u32 deadline = c.AllocQuad(static_cast<i64>(SystemTime(c)) + 100 * kTicksPerMs);
    const auto start = std::chrono::steady_clock::now();
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {event, 1, 0, deadline}) == kTimeout);
    const i64 elapsed = MillisSince(start);
    CHECK(elapsed >= 80);
    CHECK(elapsed < 2000);
    c.Kernel("NtClose", {event});
  });
}

REX_TEST_CASE("waits: KeDelayExecutionThread honors an absolute deadline", "[kernel][waits]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto start = std::chrono::steady_clock::now();
    CHECK(c.Kernel("KeDelayExecutionThread",
                   {1, 0, c.AllocQuad(static_cast<i64>(SystemTime(c)) + 100 * kTicksPerMs)}) == 0);
    i64 elapsed = MillisSince(start);
    CHECK(elapsed >= 80);
    CHECK(elapsed < 2000);
    start = std::chrono::steady_clock::now();
    CHECK(c.Kernel("KeDelayExecutionThread",
                   {1, 0, c.AllocQuad(static_cast<i64>(SystemTime(c)) - 100 * kTicksPerMs)}) == 0);
    CHECK(MillisSince(start) < 500);
  });
}

REX_TEST_CASE("waits: NetDll_WSAWaitForMultipleEvents waits its millisecond timeout",
              "[kernel][waits][net]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 unsignaled = Event(c, 0);
    const u32 signaled = Event(c, 1);
    const u32 events = c.Alloc(4);
    c.Store32(events, unsignaled);
    const auto start = std::chrono::steady_clock::now();
    CHECK(c.XAM("NetDll_WSAWaitForMultipleEvents", {1, events, 0, 100, 0}) == kTimeout);
    const i64 elapsed = MillisSince(start);
    CHECK(elapsed >= 80);
    CHECK(elapsed < 2000);
    c.Store32(events, signaled);
    CHECK(c.XAM("NetDll_WSAWaitForMultipleEvents", {1, events, 0, 100, 0}) == 0);
    c.Kernel("NtClose", {unsignaled});
    c.Kernel("NtClose", {signaled});
  });
}

}  // namespace wait_tests

namespace events_tests {

using rex::kernel::X_STATUS;

using rex::test::Caller;
using rex::test::KernelFixture;

namespace {
constexpr u32 kInvalidHandle = rex::kernel::X_STATUS_INVALID_HANDLE;
constexpr u32 kTimeout = rex::kernel::X_STATUS_TIMEOUT;
u32 Poll(Caller& c) {
  return c.AllocQuad(0);
}
}  // namespace

REX_TEST_CASE("events: manual reset create set wait clear", "[kernel][events]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    CHECKED_ELSE(handle != 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
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

REX_TEST_CASE("events: auto reset is consumed by one wait", "[kernel][events]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 1, 1}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == kTimeout);
    CHECK(c.Kernel("NtSetEvent", {handle, 0}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == kTimeout);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("events: query reports type and state", "[kernel][events]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 out = c.Alloc(8);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 1}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 manual = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtQueryEvent", {manual, out}) == 0);
    CHECK(c.Load32(out + 4) == 1);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 1, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 sync = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtQueryEvent", {sync, out}) == 0);
    CHECK(c.Load32(out + 4) == 0);
    CHECK(c.Kernel("NtQueryEvent", {0xF8001230, out}) == kInvalidHandle);
    c.Kernel("NtClose", {manual});
    c.Kernel("NtClose", {sync});
  });
}

REX_TEST_CASE("events: pulse and invalid handles", "[kernel][events]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    const u32 previous = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
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

REX_TEST_CASE("events: guest KEVENT initialized and lazily created", "[kernel][events][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 initialized = c.Alloc(0x10);
    c.Kernel("KeInitializeEvent", {initialized, 1, 1});
    CHECK(c.Load32(initialized + 8) == initialized + 8);
    CHECK(c.Load32(initialized + 12) == initialized + 8);
    CHECK(c.Kernel("KeWaitForSingleObject", {initialized, 0, 0, 0, Poll(c)}) == 0);
    CHECK(c.Kernel("KeWaitForSingleObject", {initialized, 0, 0, 0, Poll(c)}) == kTimeout);
    CHECK(c.Kernel("KeSetEvent", {initialized, 0, 0}) == 1);
    CHECK(c.Kernel("KeWaitForSingleObject", {initialized, 0, 0, 0, Poll(c)}) == 0);
    CHECK(c.Kernel("KeResetEvent", {initialized}) == 1);

    const u32 inlined = c.Alloc(0x10);
    c.Store32(inlined, 0x01000000);
    c.Store32(inlined + 4, 1);
    CHECK(c.Kernel("KeWaitForSingleObject", {inlined, 0, 0, 0, Poll(c)}) == 0);
    c.Kernel("KeSetEvent", {inlined, 0, 0});
    CHECK(c.Kernel("KeWaitForSingleObject", {inlined, 0, 0, 0, Poll(c)}) == 0);
  });
}

REX_TEST_CASE("events: a block re-initialized as a semaphore can be waited on",
              "[kernel][events]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 reused = c.Alloc(32);
    c.Kernel("KeInitializeEvent", {reused, 0, 0});
    c.Kernel("KeInitializeSemaphore", {reused, 1, 1});
    CHECK(c.Kernel("KeWaitForSingleObject", {reused, 0, 0, 0, Poll(c)}) == 0);
    CHECK(c.Kernel("KeWaitForSingleObject", {reused, 0, 0, 0, Poll(c)}) == kTimeout);
  });
}

}  // namespace events_tests

namespace reinit_tests {

using rex::u32;

u32 CreateNt(Caller& c, const char* name, u64 first = 0, u64 second = 0, u64 third = 0) {
  const u32 handle_ptr = c.Alloc(4);
  CHECKED_ELSE(c.Kernel(name, {handle_ptr, first, second, third}) == 0) {
    FAIL_CHECK("the check above failed");
    return 0;
  }
  return c.Load32(handle_ptr);
}

u32 BodyOf(Caller& c, u32 handle) {
  const u32 object_ptr = c.Alloc(4);
  CHECKED_ELSE(c.Kernel("ObReferenceObjectByHandle", {handle, 0, object_ptr}) == 0) {
    FAIL_CHECK("the check above failed");
    return 0;
  }
  return c.Load32(object_ptr);
}

u32 WaitHandleFor(Caller& c, u32 handle, i64 timeout) {
  return c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, c.AllocQuad(timeout)});
}

u32 WaitBodyFor(Caller& c, u32 body, i64 timeout) {
  return c.Kernel("KeWaitForSingleObject", {body, 0, 0, 0, c.AllocQuad(timeout)});
}

REX_TEST_CASE("NtCreateEvent gives the handle a guest body with an event header",
              "[kernel][events][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 notification = BodyOf(c, CreateNt(c, "NtCreateEvent", 0, 0, 1));
    REQUIRE(notification != 0);
    CHECK(c.Load8(notification) == 0);
    CHECK(c.Load32(notification + 4) == 1);
    CHECK(c.Load32(notification + 8) == notification + 8);
    CHECK(c.Load32(notification + 0xC) == notification + 8);
    const u32 synchronization = BodyOf(c, CreateNt(c, "NtCreateEvent", 0, 1, 0));
    REQUIRE(synchronization != 0);
    CHECK(c.Load8(synchronization) == 1);
    CHECK(c.Load32(synchronization + 4) == 0);
  });
}

REX_TEST_CASE("NtCreateSemaphore gives the handle a guest body with a semaphore header",
              "[kernel][semaphores][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 body = BodyOf(c, CreateNt(c, "NtCreateSemaphore", 0, 2, 5));
    REQUIRE(body != 0);
    CHECK(c.Load8(body) == 5);
    CHECK(c.Load32(body + 4) == 2);
    CHECK(c.Load32(body + 0x10) == 5);
    CHECK(c.Load32(body + 8) == body + 8);
    CHECK(c.Load32(body + 0xC) == body + 8);
  });
}

REX_TEST_CASE("NtCreateMutant gives the handle a guest body with a mutant header",
              "[kernel][mutants][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 free_body = BodyOf(c, CreateNt(c, "NtCreateMutant", 0, 0));
    REQUIRE(free_body != 0);
    CHECK(c.Load8(free_body) == static_cast<u32>(rex::kernel::kDispatcherMutant));
    CHECK(c.Load32(free_body + 4) == 1);
    CHECK(c.Load32(free_body + 0x18) == 0);
    CHECK(c.Load32(free_body + 8) == free_body + 8);
    CHECK(c.Load32(free_body + 0xC) == free_body + 8);
    const u32 owned_body = BodyOf(c, CreateNt(c, "NtCreateMutant", 0, 1));
    REQUIRE(owned_body != 0);
    CHECK(c.Load8(owned_body) == static_cast<u32>(rex::kernel::kDispatcherMutant));
    CHECK(c.Load32(owned_body + 4) == 0);
    CHECK(c.Load32(owned_body + 0x18) != 0);
  });
}

REX_TEST_CASE("NtCreateTimer gives the handle a guest body with a timer header",
              "[kernel][timers][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    for (const u32 type : {0u, 1u}) {
      const u32 body = BodyOf(c, CreateNt(c, "NtCreateTimer", 0, type));
      REQUIRE(body != 0);
      CHECK(c.Load8(body) == static_cast<u32>(rex::kernel::kDispatcherManualResetTimer) + type);
      CHECK(c.Load32(body + 4) == 0);
      CHECK(c.Load32(body + 8) == body + 8);
      CHECK(c.Load32(body + 0xC) == body + 8);
    }
  });
}

REX_TEST_CASE("A thread body starts with an empty wait list", "[kernel][threads][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 thread = c.Spawn([](Caller&) {});
    const u32 body = BodyOf(c, thread);
    REQUIRE(body != 0);
    CHECK(c.Load32(body + 8) == body + 8);
    CHECK(c.Load32(body + 0xC) == body + 8);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.AllocQuad(-50000000)}) == 0);
    c.Kernel("NtClose", {thread});
  });
}

REX_TEST_CASE(
    "A KeInitializeSemaphore over an NtCreateSemaphore body resets the count of the same object",
    "[kernel][semaphores][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle = CreateNt(c, "NtCreateSemaphore", 0, 2, 2);
    const u32 body = BodyOf(c, handle);
    auto& table = c.GetRuntime().GetObjectTable();
    const auto before = table.Get(handle);
    REQUIRE(before);
    c.Kernel("KeInitializeSemaphore", {body, 0, 2});
    CHECK(WaitHandleFor(c, handle, 0) == kTimeout);
    CHECK(WaitBodyFor(c, body, 0) == kTimeout);
    const auto by_body = table.GetByAddress(body);
    REQUIRE(by_body);
    CHECK(&by_body.GetKernelObject() == &before.GetKernelObject());
    CHECK(c.Kernel("NtReleaseSemaphore", {handle, 1, c.Alloc(4)}) == 0);
    CHECK(WaitBodyFor(c, body, 0) == 0);
    CHECK(WaitHandleFor(c, handle, 0) == kTimeout);
  });
}

REX_TEST_CASE("A KeInitializeMutant over an owned NtCreateMutant body leaves it free to acquire",
              "[kernel][mutants][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle = CreateNt(c, "NtCreateMutant", 0, 1);
    const u32 body = BodyOf(c, handle);
    c.Kernel("KeInitializeMutant", {body, 0});
    auto taken = std::make_shared<std::promise<u32>>();
    auto done = taken->get_future();
    const u32 other = c.Spawn([handle, taken](Caller& o) {
      taken->set_value(o.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, o.AllocQuad(-10000000)}));
    });
    CHECK(done.get() == 0);
    c.Kernel("NtClose", {other});
    c.Kernel("NtClose", {handle});
  });
}

REX_TEST_CASE(
    "A KeInitializeEvent of another type over an NtCreateEvent body resets the same object",
    "[kernel][events][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle = CreateNt(c, "NtCreateEvent", 0, 0, 1);
    const u32 body = BodyOf(c, handle);
    auto& table = c.GetRuntime().GetObjectTable();
    const auto before = table.Get(handle);
    REQUIRE(before);
    c.Kernel("KeInitializeEvent", {body, 1, 0});
    CHECK(WaitHandleFor(c, handle, 0) == kTimeout);
    CHECK(WaitBodyFor(c, body, 0) == kTimeout);
    c.Kernel("KeSetEvent", {body, 0, 0});
    CHECK(WaitHandleFor(c, handle, 0) == 0);
    CHECK(WaitHandleFor(c, handle, 0) == kTimeout);
    const auto by_body = table.GetByAddress(body);
    REQUIRE(by_body);
    CHECK(&by_body.GetKernelObject() == &before.GetKernelObject());
    const auto by_handle = table.Get(handle);
    REQUIRE(by_handle);
    CHECK(&by_handle.GetKernelObject() == &before.GetKernelObject());
  });
}

REX_TEST_CASE("A KeInitializeTimerEx over an armed NtCreateTimer body never signals",
              "[kernel][timers][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle = CreateNt(c, "NtCreateTimer");
    const u32 body = BodyOf(c, handle);
    CHECK(c.Kernel("NtSetTimerEx", {handle, c.AllocQuad(-100000), 0, 1, 0, 0, 0, 0}) == 0);
    c.Kernel("KeInitializeTimerEx", {body, 0, 0});
    CHECK(WaitHandleFor(c, handle, -3000000) == kTimeout);
    CHECK(WaitBodyFor(c, body, 0) == kTimeout);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("A thread waiting on a replaced event stays blocked and still stops on RequestStop",
              "[kernel][events][shutdown]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle = CreateNt(c, "NtCreateEvent");
    const u32 body = BodyOf(c, handle);
    auto waiting = std::make_shared<std::atomic<bool>>(false);
    auto woken = std::make_shared<std::atomic<bool>>(false);
    const u32 thread = c.Spawn([handle, waiting, woken](Caller& t) {
      waiting->store(true);
      t.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, 0});
      woken->store(true);
    });
    while (!waiting->load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    c.Kernel("KeInitializeEvent", {body, 1, 0});
    c.Kernel("NtSetEvent", {handle, 0});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(woken->load());
    auto pin = c.GetRuntime().GetThreadRegistry().FindByHandle(thread);
    REQUIRE(pin);
    pin->RequestStop();
    CHECK(WaitHandleFor(c, thread, -50000000) == 0);
    c.Kernel("NtClose", {thread});
  });
}

struct MutantHandoff {
  std::promise<u32> handle;
  std::promise<u32> release;
  std::atomic<bool> proceed{false};
};

REX_TEST_CASE("A KeReleaseMutant by the old owner after a KeInitializeMutant is refused",
              "[kernel][mutants][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto handoff = std::make_shared<MutantHandoff>();
    auto published = handoff->handle.get_future();
    auto released = handoff->release.get_future();
    const u32 owner = c.Spawn([handoff](Caller& o) {
      const u32 handle = CreateNt(o, "NtCreateMutant", 0, 1);
      handoff->handle.set_value(handle);
      while (!handoff->proceed.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      handoff->release.set_value(o.Kernel("KeReleaseMutant", {BodyOf(o, handle), 0, 0, 0}));
    });
    const u32 handle = published.get();
    c.Kernel("KeInitializeMutant", {BodyOf(c, handle), 0});
    handoff->proceed.store(true);
    CHECK(released.get() == rex::kernel::X_STATUS_MUTANT_NOT_OWNED);
    c.Kernel("NtClose", {owner});
  });
}

REX_TEST_CASE("A waiter woken on a replaced mutant leaves the new owner in place",
              "[kernel][mutants][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto handoff = std::make_shared<MutantHandoff>();
    auto published = handoff->handle.get_future();
    const u32 owner = c.Spawn([handoff](Caller& o) {
      handoff->handle.set_value(CreateNt(o, "NtCreateMutant", 0, 1));
      while (!handoff->proceed.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    const u32 handle = published.get();
    const u32 body = BodyOf(c, handle);
    auto waiting = std::make_shared<std::atomic<bool>>(false);
    auto woken = std::make_shared<std::atomic<bool>>(false);
    const u32 waiter = c.Spawn([handle, waiting, woken](Caller& w) {
      waiting->store(true);
      w.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, 0});
      woken->store(true);
    });
    while (!waiting->load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    c.Kernel("KeInitializeMutant", {body, 1});
    handoff->proceed.store(true);
    CHECK(WaitHandleFor(c, owner, -50000000) == 0);
    for (int i = 0; i < 1000 && !woken->load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(woken->load());
    CHECK(c.Kernel("KeReleaseMutant", {body, 0, 0, 0}) == 0);
    c.Kernel("NtClose", {owner});
    c.Kernel("NtClose", {waiter});
  });
}

REX_TEST_CASE("KeSetEvent and NtSetEvent from other threads run safely during a reinitialization",
              "[kernel][events][identity]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle = CreateNt(c, "NtCreateEvent", 0, 0, 0);
    const u32 body = BodyOf(c, handle);
    auto stop = std::make_shared<std::atomic<bool>>(false);
    const u32 setter = c.Spawn([handle, body, stop](Caller& t) {
      while (!stop->load()) {
        t.Kernel("KeSetEvent", {body, 0, 0});
        t.Kernel("NtSetEvent", {handle, 0});
      }
    });
    const u32 poller = c.Spawn([handle, stop](Caller& t) {
      while (!stop->load()) {
        t.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, t.AllocQuad(-1000)});
        t.Kernel("NtClearEvent", {handle, 0});
      }
    });
    for (u32 i = 0; i < 500; ++i) {
      c.Kernel("KeInitializeEvent", {body, i & 1, 0});
    }
    stop->store(true);
    CHECK(WaitHandleFor(c, setter, -50000000) == 0);
    CHECK(WaitHandleFor(c, poller, -50000000) == 0);
    c.Kernel("NtClose", {setter});
    c.Kernel("NtClose", {poller});
  });
}

}  // namespace reinit_tests
