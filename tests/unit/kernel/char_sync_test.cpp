/**
 * @file        tests/unit/kernel/char_sync_test.cpp
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

#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/xtimer.h>
#include <rex/runtime/xtypes.h>

#include "kernel_fixture.h"
#include "runtime/dispatcher.h"

using rex::X_STATUS;

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {
constexpr u32 kInvalidHandle = rex::X_STATUS_INVALID_HANDLE;
constexpr u32 kTimeout = rex::X_STATUS_TIMEOUT;
u32 Poll(Caller& c) {
  return c.Quad(0);
}
}  // namespace

REX_TEST_CASE("semaphores: create release wait and limit", "[char][semaphores]") {
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
          rex::X_STATUS_SEMAPHORE_LIMIT_EXCEEDED);
    CHECK(c.Kernel("NtReleaseSemaphore", {0xF8001230, 1, previous}) == kInvalidHandle);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("semaphores: guest KSEMAPHORE", "[char][semaphores][identity]") {
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

REX_TEST_CASE("mutants: recursive ownership and release", "[char][mutants]") {
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
    CHECK(c.Kernel("NtReleaseMutant", {handle, 0}) == rex::X_STATUS_MUTANT_NOT_OWNED);
    CHECK(c.Kernel("NtReleaseMutant", {0xF8001230, 0}) == kInvalidHandle);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("mutants: another thread cannot take an owned mutant", "[char][mutants]") {
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
      other_result->set_value(o.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, o.Quad(-10000)}));
    });
    CHECK(other_done.get() == kTimeout);
    CHECK(c.Kernel("NtReleaseMutant", {handle, 0}) == 0);
    c.Kernel("NtClose", {other});
    c.Kernel("NtClose", {handle});
  });
}

REX_TEST_CASE("timers: create set wait cancel", "[char][timers]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateTimer", {handle_ptr, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, Poll(c)}) == kTimeout);
    CHECK(c.Kernel("NtSetTimerEx", {handle, c.Quad(-100000), 0, 1, 0, 0, 0, 0}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {handle, 1, 0, c.Quad(-5000000)}) == 0);
    CHECK(c.Kernel("NtCancelTimer", {handle, 0}) == 0);
    CHECK(c.Kernel("NtSetTimerEx", {handle, c.Quad(-100000), 0, 1, 0, 1, 0, 0}) ==
          rex::X_STATUS_TIMER_RESUME_IGNORED);
    CHECK(c.Kernel("NtSetTimerEx", {0xF8001230, c.Quad(-100000), 0, 1, 0, 0, 0, 0}) ==
          kInvalidHandle);
    CHECK(c.Kernel("NtClose", {handle}) == 0);
  });
}

REX_TEST_CASE("timers: completion routine runs as an APC on the setting thread",
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
    CHECKED_ELSE(c.Kernel("NtCreateTimer", {handle_ptr, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 handle = c.Load32(handle_ptr);
    CHECKED_ELSE(c.Kernel("NtSetTimerEx", {handle, c.Quad(-100000), routine, 1, 0, 0, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 event_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateEvent", {event_ptr, 0, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {c.Load32(event_ptr), 1, 1, c.Quad(-5000000)}) ==
          rex::X_STATUS_USER_APC);
    CHECK(g_routine_calls == 1);
    c.Kernel("NtClose", {handle});
    c.Kernel("NtClose", {c.Load32(event_ptr)});
  });
}

namespace {

u32 WaitObject(Caller& c, u32 object, i64 timeout) {
  return c.Kernel("KeWaitForSingleObject", {object, 0, 0, 0, c.Quad(timeout)});
}

u32 WaitAll(Caller& c, std::initializer_list<u32> objects, i64 timeout) {
  const u32 array = c.Alloc(static_cast<u32>(objects.size() * 4));
  u32 offset = 0;
  for (const u32 object : objects) {
    c.Store32(array + offset, object);
    offset += 4;
  }
  return c.Kernel("KeWaitForMultipleObjects",
                  {static_cast<u32>(objects.size()), array, 0, 0, 1, 0, c.Quad(timeout), 0});
}

}  // namespace

REX_TEST_CASE("mutants: a Ke mutant initialized with an owner is owned by the caller",
              "[char][mutants]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 mutant = c.Alloc(0x20);
    c.Kernel("KeInitializeMutant", {mutant, 1});
    CHECK(c.Load32(mutant + 4) == 0);
    CHECK(c.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == 0);
    CHECK(c.Load32(mutant + 4) == 1);
  });
}

REX_TEST_CASE("mutants: a Ke mutant counts recursive acquisitions", "[char][mutants]") {
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
              "[char][mutants]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 mutant = c.Alloc(0x20);
    c.Kernel("KeInitializeMutant", {mutant, 1});
    const u32 thread = c.Spawn([mutant](Caller& o) {
      CHECK(o.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == rex::X_STATUS_MUTANT_NOT_OWNED);
    });
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.Quad(-50000000)}) == 0);
    c.Kernel("NtClose", {thread});
    CHECK(c.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == 0);
  });
}

REX_TEST_CASE("mutants: a wait all takes the Ke mutant only when every object is signaled",
              "[char][mutants]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 mutant = c.Alloc(0x20);
    c.Kernel("KeInitializeMutant", {mutant, 0});
    const u32 event = c.Alloc(0x10);
    c.Kernel("KeInitializeEvent", {event, 0, 0});
    const u32 thread = c.Spawn([mutant, event](Caller& o) {
      CHECK(WaitAll(o, {mutant, event}, -500000) == kTimeout);
      CHECK(o.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == rex::X_STATUS_MUTANT_NOT_OWNED);
      o.Kernel("KeSetEvent", {event, 0, 0});
      CHECK(WaitAll(o, {mutant, event}, -10000000) == 0);
      CHECK(o.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == 0);
    });
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.Quad(-50000000)}) == 0);
    c.Kernel("NtClose", {thread});
  });
}

REX_TEST_CASE("mutants: a Ke mutant release hands ownership to 1 waiter", "[char][mutants]") {
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
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {first, 1, 0, c.Quad(-50000000)}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {second, 1, 0, c.Quad(-50000000)}) == 0);
    CHECK(acquired->load() == 2);
    c.Kernel("NtClose", {first});
    c.Kernel("NtClose", {second});
  });
}

REX_TEST_CASE("events: KeInitializeEvent on an initialized event reuses its slot",
              "[char][events]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event = c.Alloc(0x10);
    c.Kernel("KeInitializeEvent", {event, 0, 1});
    const u32 stash = c.Load32(event + 0xC);
    c.Kernel("KeInitializeEvent", {event, 0, 0});
    CHECK(c.Load32(event + 0xC) == stash);
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

u64 SystemTime(Caller& c) {
  const u32 time = c.Alloc(8);
  c.Kernel("KeQuerySystemTime", {time});
  return (static_cast<u64>(c.Load32(time)) << 32) | c.Load32(time + 4);
}

u32 KeTimer(Caller& c, u32 type) {
  const u32 timer = c.Alloc(0x28);
  c.Kernel("KeInitializeTimerEx", {timer, type, 0});
  return timer;
}

u32 WaitTimer(Caller& c, u32 timer, i64 timeout) {
  return c.Kernel("KeWaitForSingleObject", {timer, 0, 0, 0, c.Quad(timeout)});
}

}  // namespace

REX_TEST_CASE("timers: a notification Ke timer stays signaled", "[char][timers]") {
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

REX_TEST_CASE("timers: a synchronization Ke timer resets on a satisfied wait", "[char][timers]") {
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
              "[char][timers]") {
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

REX_TEST_CASE("timers: a Ke timer queues its DPC with the expiry time", "[char][timers][dpc]") {
  g_timer_dpc_runs = 0;
  g_timer_dpc_time = 0;
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 routine = c.runtime().function_dispatcher()->AllocateThunk(&TimerDPC, 0);
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

REX_TEST_CASE("timers: a periodic Ke timer rearms until canceled", "[char][timers][dpc]") {
  g_timer_dpc_runs = 0;
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 routine = c.runtime().function_dispatcher()->AllocateThunk(&TimerDPC, 0);
    const u32 dpc = c.Alloc(0x20);
    c.Kernel("KeInitializeDpc", {dpc, routine, 0});
    const u32 timer = KeTimer(c, 1);
    CHECK(c.Kernel("KeSetTimerEx", {timer, static_cast<u64>(-100000), 20, dpc}) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(c.Kernel("KeCancelTimer", {timer}) == 1);
    const int runs = g_timer_dpc_runs;
    CHECK(runs >= 3);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(g_timer_dpc_runs <= runs + 1);
  });
}

REX_TEST_CASE("interrupts: connect, double connect and disconnect", "[char][interrupts]") {
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

REX_TEST_CASE("interrupts: KeQueryInterruptTime grows in 100 ns units", "[char][interrupts]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    c.Kernel("KeQueryInterruptTime");
    const u64 first = c.ctx().r3.u64;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    c.Kernel("KeQueryInterruptTime");
    const u64 second = c.ctx().r3.u64;
    CHECK(second - first >= 400000);
    CHECK(second - first < 50000000);
  });
}

REX_TEST_CASE("timers: a Ke timer with an absolute due time of 0 expires at once",
              "[char][timers]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 timer = KeTimer(c, 0);
    CHECK(c.Kernel("KeSetTimer", {timer, 0, 0}) == 0);
    CHECK(WaitTimer(c, timer, -10000000) == 0);
  });
}

REX_TEST_CASE("timers: stopping the dispatcher disarms Ke timer expiries", "[char][timers]") {
  KernelFixture fixture;
  u32 timer = 0;
  fixture.RunOnGuestThread([&timer](Caller& c) {
    timer = KeTimer(c, 1);
    CHECK(c.Kernel("KeSetTimerEx", {timer, static_cast<u64>(-100000), 10, 0}) == 0);
  });
  static_cast<rex::runtime::Dispatcher&>(fixture.runtime().dispatcher())
      .Stop(std::chrono::milliseconds(1000));
  auto* object = fixture.runtime().memory()->TranslateVirtual<rex::runtime::X_KTIMER*>(timer);
  object->header.signal_state = 0;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(static_cast<u32>(object->header.signal_state) == 0);
}

REX_TEST_CASE("mutants: a Ke mutant abandoned by its owner is released by the next owner",
              "[char][mutants]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 mutant = c.Alloc(0x20);
    c.Kernel("KeInitializeMutant", {mutant, 0});
    const u32 thread =
        c.Spawn([mutant](Caller& o) { CHECK(WaitObject(o, mutant, -10000000) == 0); });
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.Quad(-50000000)}) == 0);
    c.Kernel("NtClose", {thread});
    CHECK(WaitObject(c, mutant, -10000000) == rex::X_STATUS_ABANDONED_WAIT_0);
    CHECK(c.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == 0);
  });
}

REX_TEST_CASE(
    "mutants: a Ke mutant abandoned by a release from its owner is free and reports it once",
    "[char][mutants]") {
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
      CHECK(WaitObject(o, mutant, -10000000) == rex::X_STATUS_ABANDONED_WAIT_0);
      CHECK(o.Load8(mutant + 0x1C) == 0);
      CHECK(o.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == 0);
      CHECK(WaitObject(o, mutant, -10000000) == 0);
      CHECK(o.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == 0);
    });
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.Quad(-50000000)}) == 0);
    c.Kernel("NtClose", {thread});
  });
}

REX_TEST_CASE("mutants: a Ke mutant abandon by a thread that does not own it fails",
              "[char][mutants]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 mutant = c.Alloc(0x20);
    c.Kernel("KeInitializeMutant", {mutant, 1});
    const u32 thread = c.Spawn([mutant](Caller& o) {
      CHECK(o.Kernel("KeReleaseMutant", {mutant, 0, 1, 0}) == rex::X_STATUS_MUTANT_NOT_OWNED);
    });
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.Quad(-50000000)}) == 0);
    c.Kernel("NtClose", {thread});
    CHECK(c.Load32(mutant + 4) == 0);
    CHECK(c.Kernel("KeReleaseMutant", {mutant, 0, 0, 0}) == 0);
  });
}
