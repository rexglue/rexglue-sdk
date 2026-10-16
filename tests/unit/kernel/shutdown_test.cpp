/**
 * @file        tests/unit/kernel/shutdown_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <string>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/api.h>
#include <rex/filesystem.h>
#include <rex/platform/exceptions.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/thread_registry.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xthread.h>

#include "kernel_fixture.h"
#include "support/elapsed.h"

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

extern "C" void rexshim_ConvertThreadToFiber(PPCContext& ctx, rex::u8* base);
extern "C" void rexshim_CreateFiber(PPCContext& ctx, rex::u8* base);
extern "C" void rexshim_SwitchToFiber(PPCContext& ctx, rex::u8* base);

using rex::i64;
using rex::u32;
using rex::u64;
using rex::u8;

REX_RECOMPILED_CODE_SECTION extern "C" void SpinInGeneratedCode(const volatile rex::u32* release) {
  while (*release == 0) {}
}

REX_RECOMPILED_CODE_SECTION extern "C" void SpinInSEHScope(const volatile rex::u32* release) {
  REX_SEH_TRY {
    while (*release == 0) {}
  }
  REX_SEH_CATCH_ALL {
    REX_SEH_RETHROW;
  }
  REX_SEH_END
}

namespace {

using rex::test::MillisSince;

std::atomic<bool> g_extern_c_unwound{false};

struct SetOnUnwind {
  ~SetOnUnwind() { g_extern_c_unwound.store(true); }
};

}  // namespace

extern "C" void StopInsideExternC() {
  SetOnUnwind guard;
  rex::runtime::KernelThread::CheckStop();
}

namespace {

void (*volatile g_stop_inside_extern_c)() = &StopInsideExternC;
void (*volatile g_spin_in_generated_code)(const volatile rex::u32*) = &SpinInGeneratedCode;
void (*volatile g_spin_in_seh_scope)(const volatile rex::u32*) = &SpinInSEHScope;

}  // namespace

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {

using namespace std::chrono_literals;

constexpr auto kStopBudget = 500ms;

u32 CreateEvent(Caller& c) {
  const u32 handle_ptr = c.Alloc(4);
  CHECKED_ELSE(c.Kernel("NtCreateEvent", {handle_ptr, 0, 0, 0}) == 0) {
    FAIL_CHECK("the check above failed");
    return 0;
  }
  return c.Load32(handle_ptr);
}

u32 WaitForever(Caller& c, u32 handle, u32 alertable) {
  return c.Kernel("NtWaitForSingleObjectEx", {handle, 1, alertable, 0});
}

bool Joined(Caller& c, u32 thread, std::chrono::milliseconds budget) {
  const i64 ticks = -static_cast<i64>(budget.count()) * 10000;
  return c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.Quad(ticks)}) == 0;
}

void RequestStop(Caller& c, u32 thread) {
  auto pin = c.GetRuntime().GetThreadRegistry().FindByHandle(thread);
  CHECKED_ELSE(pin) {
    FAIL_CHECK("the check above failed");
    return;
  }
  pin->RequestStop();
}

void WaitUntil(const std::atomic<bool>& flag) {
  const auto until = std::chrono::steady_clock::now() + 2s;
  while (!flag.load() && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(1ms);
  }
  CHECKED_ELSE(flag.load()) {
    FAIL_CHECK("the check above failed");
    return;
  }
}

struct SetOnDestroy {
  std::shared_ptr<std::atomic<bool>> flag;
  ~SetOnDestroy() { flag->store(true); }
};

void GuardedWait(Caller& t, u32 event, std::atomic<bool>& handled,
                 std::atomic<bool>& waiting){REX_SEH_TRY{waiting.store(true);
WaitForever(t, event, 1);
}  // namespace
REX_SEH_CATCH_ALL {
  handled.store(true);
  REX_SEH_RETHROW;
}
REX_SEH_END
}

std::atomic<u32> g_fiber_event{0};
std::atomic<bool> g_fiber_waiting{false};

void FiberWait(PPCContext& ctx, u8* base) {
  Caller f(ctx, base, *rex::runtime::ThreadState::Get()->GetRuntime());
  g_fiber_waiting.store(true);
  WaitForever(f, g_fiber_event.load(), 1);
}

u32 SpawnFiberWaiter(Caller& c, u32 fiber_entry) {
  g_fiber_event.store(CreateEvent(c));
  g_fiber_waiting.store(false);
  return c.Spawn([fiber_entry](Caller& t) {
    PPCContext& ctx = t.GetCtx();
    u8* base = t.GetRuntime().GetMemory()->GetVirtualMembase();
    ctx.r3.u64 = 0;
    rexshim_ConvertThreadToFiber(ctx, base);
    ctx.r3.u64 = 0x10000;
    ctx.r4.u64 = fiber_entry;
    ctx.r5.u64 = 0;
    rexshim_CreateFiber(ctx, base);
    ctx.r3.u64 = ctx.r3.u32;
    rexshim_SwitchToFiber(ctx, base);
  });
}

std::atomic<bool> g_apc_ran{false};

void MarkAPC(PPCContext&, u8*) {
  g_apc_ran.store(true);
}

u32 StopsWithin(Caller& c, const std::function<void(Caller&)>& body) {
  auto started = std::make_shared<std::atomic<bool>>(false);
  const u32 thread = c.Spawn([started, body](Caller& t) {
    started->store(true);
    body(t);
  });
  WaitUntil(*started);
  std::this_thread::sleep_for(20ms);
  RequestStop(c, thread);
  return thread;
}

bool StoppedWithin(rex::Runtime& runtime, u32 thread, std::chrono::milliseconds budget) {
  const auto until = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < until) {
    auto pin = runtime.GetThreadRegistry().FindByHandle(thread);
    if (!pin || !pin->IsRunning()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

bool ExitedWithin(rex::Runtime& runtime, u32 thread, std::chrono::milliseconds budget) {
  const auto until = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < until) {
    auto pin = runtime.GetThreadRegistry().FindByHandle(thread);
    if (!pin || pin->GetGuestObject<rex::runtime::X_KTHREAD>()->terminated) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

std::atomic<u32> g_notify_event{0};

volatile u32 g_never_released = 0;

void WaitingTerminateRoutine(PPCContext& ctx, u8* base) {
  Caller r(ctx, base, *rex::runtime::ThreadState::Get()->GetRuntime());
  WaitForever(r, g_notify_event.load(), 0);
  ctx.r3.u64 = 0;
}

std::atomic<std::array<u8, 256>*> g_allocation_sink{nullptr};

std::atomic<bool> g_exit_wait_armed{false};

void WaitingExitRoutine(PPCContext& ctx, u8* base) {
  if (ctx.r3.u32 == 0 && g_exit_wait_armed.load()) {
    Caller r(ctx, base, *rex::runtime::ThreadState::Get()->GetRuntime());
    WaitForever(r, g_notify_event.load(), 0);
  }
}

void RegisterThreadNotification(Caller& c, u32 routine) {
  const u32 notification = c.Alloc(0xC);
  c.Store32(notification + 8, routine);
  c.XAM("XRegisterThreadNotifyRoutine", {notification, 1});
}

#if !REX_PLATFORM_WIN32
std::atomic<bool> g_scope_probe_armed{false};
std::atomic<bool> g_scope_probe_ran{false};
std::atomic<bool> g_scope_left_open{false};

void ProbeScopeOnExit(PPCContext& ctx, u8*) {
  if (ctx.r3.u32 == 0 && g_scope_probe_armed.load()) {
    g_scope_probe_ran.store(true);
    g_scope_left_open.store(rex::platform::GetSEHThreadState().top != nullptr);
  }
}

std::atomic<bool> g_fiber_probed{false};
std::atomic<bool> g_fiber_saw_scope{false};

void ProbeScopeOnFiber(PPCContext&, u8*) {
  g_fiber_saw_scope.store(rex::platform::GetSEHThreadState().top != nullptr);
  g_fiber_probed.store(true);
}

bool SwitchToFiberInScope(Caller& t, u32 fiber_entry) {
  bool kept = false;
  REX_SEH_TRY {
    PPCContext& ctx = t.GetCtx();
    u8* base = t.GetRuntime().GetMemory()->GetVirtualMembase();
    rex::platform::SEHScope* const scope = rex::platform::GetSEHThreadState().top;
    ctx.r3.u64 = 0;
    rexshim_ConvertThreadToFiber(ctx, base);
    ctx.r3.u64 = 0x10000;
    ctx.r4.u64 = fiber_entry;
    ctx.r5.u64 = 0;
    rexshim_CreateFiber(ctx, base);
    ctx.r3.u64 = ctx.r3.u32;
    rexshim_SwitchToFiber(ctx, base);
    kept = rex::platform::GetSEHThreadState().top == scope;
  }
  REX_SEH_CATCH_ALL {
    REX_SEH_RETHROW;
  }
  REX_SEH_END
  return kept;
}
#endif

}  // namespace

REX_TEST_CASE("ExTerminateThread from a nested call runs host destructors", "[kernel][shutdown]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto destroyed = std::make_shared<std::atomic<bool>>(false);
    const u32 thread = c.Spawn([destroyed](Caller& t) {
      SetOnDestroy guard{destroyed};
      t.Kernel("ExTerminateThread", {7});
    });
    CHECK(Joined(c, thread, kStopBudget));
    CHECK(destroyed->load());
  });
}

REX_TEST_CASE("A stop crosses a guest SEH scope without running its handler",
              "[kernel][shutdown]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event = CreateEvent(c);
    auto handled = std::make_shared<std::atomic<bool>>(false);
    auto waiting = std::make_shared<std::atomic<bool>>(false);
    const u32 thread = c.Spawn(
        [event, handled, waiting](Caller& t) { GuardedWait(t, event, *handled, *waiting); });
    WaitUntil(*waiting);
    RequestStop(c, thread);
    CHECK(Joined(c, thread, kStopBudget));
    CHECK_FALSE(handled->load());
  });
}

REX_TEST_CASE("A stop on a guest fiber unwinds the thread", "[kernel][shutdown]") {
  KernelFixture fixture;
  const u32 fiber_entry =
      fixture.GetRuntime().GetFunctionDispatcher()->AllocateThunk(&FiberWait, 0);
  REQUIRE(fiber_entry != 0);
  fixture.RunOnGuestThread([fiber_entry](Caller& c) {
    const u32 thread = SpawnFiberWaiter(c, fiber_entry);
    WaitUntil(g_fiber_waiting);
    RequestStop(c, thread);
    CHECK(Joined(c, thread, kStopBudget));
  });
}

REX_TEST_CASE("A guest thread that ends as a fiber frees its fibers once",
              "[kernel][shutdown][fiber]") {
  KernelFixture fixture;
  const u32 fiber_entry =
      fixture.GetRuntime().GetFunctionDispatcher()->AllocateThunk(&FiberWait, 0);
  REQUIRE(fiber_entry != 0);
  fixture.RunOnGuestThread([fiber_entry](Caller& c) {
    const u32 thread = SpawnFiberWaiter(c, fiber_entry);
    WaitUntil(g_fiber_waiting);
    c.Kernel("NtSetEvent", {g_fiber_event.load(), 0});
    CHECK(Joined(c, thread, kStopBudget));
    c.Kernel("NtClose", {thread});
  });
}

REX_TEST_CASE("A stop wakes a non-alertable wait on an event nobody sets", "[kernel][shutdown]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event = CreateEvent(c);
    const u32 thread = StopsWithin(c, [event](Caller& t) { WaitForever(t, event, 0); });
    CHECK(Joined(c, thread, kStopBudget));
  });
}

REX_TEST_CASE("A stop wakes a wait all on events not all set", "[kernel][shutdown]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handles = c.Alloc(8);
    c.Store32(handles, CreateEvent(c));
    c.Store32(handles + 4, CreateEvent(c));
    c.Kernel("NtSetEvent", {c.Load32(handles), 0});
    const u32 thread = StopsWithin(c, [handles](Caller& t) {
      t.Kernel("NtWaitForMultipleObjectsEx", {2, handles, 0, 1, 0, 0});
    });
    CHECK(Joined(c, thread, kStopBudget));
  });
}

REX_TEST_CASE("A stop wakes a signal and wait", "[kernel][shutdown]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 signal = CreateEvent(c);
    const u32 wait = CreateEvent(c);
    const u32 thread = StopsWithin(c, [signal, wait](Caller& t) {
      t.Kernel("NtSignalAndWaitForSingleObjectEx", {signal, wait, 0, 0, 0});
    });
    CHECK(Joined(c, thread, kStopBudget));
  });
}

REX_TEST_CASE("A stop wakes a non-alertable delay of 1 hour", "[kernel][shutdown]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 thread = StopsWithin(
        c, [](Caller& t) { t.Kernel("KeDelayExecutionThread", {1, 0, t.Quad(-36000000000LL)}); });
    CHECK(Joined(c, thread, kStopBudget));
  });
}

REX_TEST_CASE("A stop releases a host lock held across a wait", "[kernel][shutdown]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event = CreateEvent(c);
    auto mutex = std::make_shared<std::mutex>();
    const u32 thread = StopsWithin(c, [event, mutex](Caller& t) {
      std::lock_guard lock(*mutex);
      WaitForever(t, event, 0);
    });
    CHECK(Joined(c, thread, kStopBudget));
    CHECK(mutex->try_lock());
    mutex->unlock();
  });
}

REX_TEST_CASE("A stop requested before the wait still wakes it", "[kernel][shutdown]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 event = CreateEvent(c);
    auto release = std::make_shared<std::atomic<bool>>(false);
    auto started = std::make_shared<std::atomic<bool>>(false);
    const u32 thread = c.Spawn([event, release, started](Caller& t) {
      started->store(true);
      while (!release->load()) {
        std::this_thread::sleep_for(1ms);
      }
      WaitForever(t, event, 0);
    });
    WaitUntil(*started);
    RequestStop(c, thread);
    release->store(true);
    CHECK(Joined(c, thread, kStopBudget));
  });
}

REX_TEST_CASE("A non-alertable wait keeps waiting through a guest APC", "[kernel][shutdown]") {
  KernelFixture fixture;
  const u32 routine = fixture.GetRuntime().GetFunctionDispatcher()->AllocateThunk(&MarkAPC, 0);
  REQUIRE(routine != 0);
  fixture.RunOnGuestThread([routine](Caller& c) {
    g_apc_ran.store(false);
    const u32 event = CreateEvent(c);
    auto status = std::make_shared<std::atomic<u32>>(0);
    auto waiting = std::make_shared<std::atomic<bool>>(false);
    auto elapsed = std::make_shared<std::atomic<i64>>(0);
    const u32 thread = c.Spawn([event, status, waiting, elapsed](Caller& t) {
      waiting->store(true);
      const auto start = std::chrono::steady_clock::now();
      status->store(t.Kernel("NtWaitForSingleObjectEx", {event, 1, 0, t.Quad(-2000000)}));
      elapsed->store(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count());
    });
    WaitUntil(*waiting);
    std::this_thread::sleep_for(20ms);
    c.Kernel("NtQueueApcThread", {thread, routine, 0, 0, 0});
    CHECK(Joined(c, thread, 2000ms));
    CHECK(status->load() == 0x102);
    CHECK(elapsed->load() >= 190);
    CHECK_FALSE(g_apc_ran.load());
  });
}

REX_TEST_CASE("TerminateTitle ends guest threads in every wait kind with no kill",
              "[kernel][shutdown]") {
  KernelFixture fixture;
  std::vector<u32> threads;
  fixture.RunOnGuestThread([&threads](Caller& c) {
    const u32 event = CreateEvent(c);
    const u32 handles = c.Alloc(8);
    c.Store32(handles, CreateEvent(c));
    c.Store32(handles + 4, event);
    c.Kernel("NtSetEvent", {c.Load32(handles), 0});
    threads.push_back(c.Spawn([event](Caller& t) { WaitForever(t, event, 0); }));
    threads.push_back(c.Spawn([handles](Caller& t) {
      t.Kernel("NtWaitForMultipleObjectsEx", {2, handles, 0, 1, 0, 0});
    }));
    threads.push_back(c.Spawn([event](Caller& t) {
      t.Kernel("NtSignalAndWaitForSingleObjectEx", {CreateEvent(t), event, 0, 0, 0});
    }));
    threads.push_back(c.Spawn(
        [](Caller& t) { t.Kernel("KeDelayExecutionThread", {1, 0, t.Quad(-36000000000LL)}); }));
    std::this_thread::sleep_for(50ms);
  });
  const auto start = std::chrono::steady_clock::now();
  fixture.GetRuntime().TerminateTitle();
  CHECK(MillisSince(start) < 1000);
  for (const u32 thread : threads) {
    CHECK(StoppedWithin(fixture.GetRuntime(), thread, 100ms));
  }
}

REX_TEST_CASE("TerminateTitle stops every thread launched while it runs", "[kernel][shutdown]") {
  struct Launched {
    std::mutex mutex;
    std::vector<u32> handles;
  };
  KernelFixture fixture;
  auto spawner = std::make_shared<std::atomic<u32>>(0);
  auto launched = std::make_shared<Launched>();
  fixture.RunOnGuestThread([spawner, launched](Caller& c) {
    const u32 event = CreateEvent(c);
    spawner->store(c.Spawn([event, launched](Caller& t) {
      const u32 handle_ptr = t.Alloc(4);
      for (u32 i = 0; i < 2000; ++i) {
        const u32 body =
            Caller::PutBodyForTest([event](Caller& child) { WaitForever(child, event, 0); });
        const u32 status = t.Kernel("ExCreateThread",
                                    {handle_ptr, 0x10000, 0, 0, Caller::start_thunk, body, i % 2});
        if (status == 0) {
          std::lock_guard lock(launched->mutex);
          launched->handles.push_back(t.Load32(handle_ptr));
        }
      }
    }));
    std::this_thread::sleep_for(20ms);
  });
  const auto start = std::chrono::steady_clock::now();
  fixture.GetRuntime().TerminateTitle();
  CHECK(MillisSince(start) < 1000);
  CHECK(StoppedWithin(fixture.GetRuntime(), spawner->load(), 100ms));
  std::lock_guard lock(launched->mutex);
  CHECK(!launched->handles.empty());
  for (const u32 handle : launched->handles) {
    CHECK(ExitedWithin(fixture.GetRuntime(), handle, 1000ms));
  }
}

REX_TEST_CASE(
    "A thread created after TerminateTitle leaves the title process thread list as it was",
    "[kernel][shutdown]") {
  KernelFixture fixture;
  fixture.GetRuntime().TerminateTitle();

  rex::runtime::ThreadState host_state(0, 0, 0, fixture.GetRuntime().GetMemory(),
                                       &fixture.GetRuntime());
  rex::runtime::ThreadState* previous = rex::runtime::ThreadState::Get();
  rex::runtime::ThreadState::Bind(&host_state);
  Caller c(*host_state.GetContext(), fixture.GetRuntime().GetMemory()->GetVirtualMembase(),
           fixture.GetRuntime());

  const u32 process = fixture.GetRuntime().GetTitleProcess();
  const u32 count = c.Load32(process + 0x14);
  const u32 flink = c.Load32(process + 0x04);
  const u32 blink = c.Load32(process + 0x08);
  const u32 handle_ptr = c.Alloc(4);
  const u32 body = Caller::PutBodyForTest([](Caller&) {});
  CHECK(c.Kernel("ExCreateThread", {handle_ptr, 0x10000, 0, 0, Caller::start_thunk, body, 0}) ==
        rex::X_STATUS_PROCESS_IS_TERMINATING);
  CHECK(c.Load32(process + 0x14) == count);
  CHECK(c.Load32(process + 0x04) == flink);
  CHECK(c.Load32(process + 0x08) == blink);

  rex::runtime::ThreadState::Bind(previous);
}

REX_TEST_CASE("TerminateTitle from a guest thread ends the caller last", "[kernel][shutdown]") {
  KernelFixture fixture;
  auto terminator = std::make_shared<std::atomic<u32>>(0);
  auto other = std::make_shared<std::atomic<u32>>(0);
  auto other_stopped_first = std::make_shared<std::atomic<bool>>(false);
  auto started = std::make_shared<std::atomic<bool>>(false);
  fixture.RunOnGuestThread([terminator, other, other_stopped_first, started](Caller& c) {
    const u32 event = CreateEvent(c);
    other->store(c.Spawn([event](Caller& t) { WaitForever(t, event, 0); }));
    terminator->store(c.Spawn([other, other_stopped_first, started](Caller& t) {
      started->store(true);
      std::this_thread::sleep_for(50ms);
      struct Last {
        Caller& t;
        u32 other;
        std::shared_ptr<std::atomic<bool>> seen;
        ~Last() {
          auto pin = t.GetRuntime().GetThreadRegistry().FindByHandle(other);
          seen->store(!pin || !pin->IsRunning());
        }
      } last{t, other->load(), other_stopped_first};
      t.GetRuntime().TerminateTitle();
    }));
  });
  WaitUntil(*started);
  CHECK(StoppedWithin(fixture.GetRuntime(), terminator->load(), 1000ms));
  CHECK(other_stopped_first->load());
}

REX_TEST_CASE("A terminate notification that waits forever is stopped with no kill",
              "[kernel][shutdown]") {
  KernelFixture fixture;
  const u32 routine =
      fixture.GetRuntime().GetFunctionDispatcher()->AllocateThunk(&WaitingTerminateRoutine, 0);
  REQUIRE(routine != 0);
  fixture.RunOnGuestThread([routine](Caller& c) {
    g_notify_event.store(CreateEvent(c));
    const u32 registration = c.Alloc(0x10);
    c.Store32(registration, routine);
    c.Store32(registration + 4, 1);
    c.Kernel("ExRegisterTitleTerminateNotification", {registration, 1});
  });
  const auto start = std::chrono::steady_clock::now();
  fixture.GetRuntime().TerminateTitle();
  const i64 elapsed = MillisSince(start);
  CHECK(elapsed >= 1900);
  CHECK(elapsed < 3500);
}

REX_TEST_CASE("TerminateTitle stops a guest thread spinning in generated code",
              "[kernel][shutdown][forced_stop]") {
  KernelFixture fixture;
  fixture.GetRuntime().GetFunctionDispatcher()->AddRecompiledCode(REX_RECOMPILED_CODE());
  auto started = std::make_shared<std::atomic<bool>>(false);
  auto unwound = std::make_shared<std::atomic<bool>>(false);
  u32 thread = 0;
  fixture.RunOnGuestThread([&thread, started, unwound](Caller& c) {
    thread = c.Spawn([started, unwound](Caller&) {
      SetOnDestroy guard{unwound};
      started->store(true);
      g_spin_in_generated_code(&g_never_released);
    });
  });
  WaitUntil(*started);
  const auto start = std::chrono::steady_clock::now();
  fixture.GetRuntime().TerminateTitle();
  CHECK(MillisSince(start) < 3000);
  CHECK(StoppedWithin(fixture.GetRuntime(), thread, 100ms));
  CHECK(unwound->load());
}

REX_TEST_CASE("TerminateTitle stops a guest thread spinning in an SEH scope",
              "[kernel][shutdown][forced_stop]") {
  KernelFixture fixture;
  fixture.GetRuntime().GetFunctionDispatcher()->AddRecompiledCode(REX_RECOMPILED_CODE());
  auto started = std::make_shared<std::atomic<bool>>(false);
  auto unwound = std::make_shared<std::atomic<bool>>(false);
  u32 thread = 0;
  fixture.RunOnGuestThread([&thread, started, unwound](Caller& c) {
    thread = c.Spawn([started, unwound](Caller&) {
      SetOnDestroy guard{unwound};
      started->store(true);
      g_spin_in_seh_scope(&g_never_released);
    });
  });
  WaitUntil(*started);
  const auto start = std::chrono::steady_clock::now();
  fixture.GetRuntime().TerminateTitle();
  CHECK(MillisSince(start) < 3000);
  CHECK(StoppedWithin(fixture.GetRuntime(), thread, 100ms));
  CHECK(unwound->load());
}

REX_TEST_CASE("A stop requested before a thread launches ends it at its first check",
              "[kernel][shutdown]") {
  KernelFixture fixture;
  auto ran_on = std::make_shared<std::atomic<bool>>(false);
  auto thread =
      std::make_unique<rex::runtime::KernelThread>(fixture.GetRuntime(), 128 * 1024, 0, [ran_on] {
        rex::runtime::KernelThread::Sleep(1h);
        ran_on->store(true);
        return 0;
      });
  thread->RequestStop();
  CHECK_FALSE(thread->ForceStop());
  auto handle = fixture.GetRuntime().GetThreadRegistry().Launch(std::move(thread));
  REQUIRE(handle);
  CHECK(StoppedWithin(fixture.GetRuntime(), *handle, 1000ms));
  CHECK_FALSE(ran_on->load());
}

REX_TEST_CASE("A thread that holds a mutant when the stop lands abandons it",
              "[kernel][shutdown]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 handle_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateMutant", {handle_ptr, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 mutant = c.Load32(handle_ptr);
    const u32 event = CreateEvent(c);
    auto owned = std::make_shared<std::atomic<bool>>(false);
    const u32 holder = c.Spawn([mutant, event, owned](Caller& t) {
      CHECKED_ELSE(t.Kernel("NtWaitForSingleObjectEx", {mutant, 1, 0, 0}) == 0) {
        FAIL_CHECK("the check above failed");
        return;
      }
      owned->store(true);
      WaitForever(t, event, 0);
    });
    WaitUntil(*owned);
    RequestStop(c, holder);
    CHECK(Joined(c, holder, 1000ms));
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {mutant, 1, 0, c.Quad(-10000000)}) ==
          rex::X_STATUS_ABANDONED_WAIT_0);
  });
}

REX_TEST_CASE("A ThreadStop unwinds through an extern C frame", "[kernel][shutdown]") {
  KernelFixture fixture;
  g_extern_c_unwound.store(false);
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 thread = c.Spawn([](Caller&) {
      rex::runtime::KernelThread::GetCurrentThread()->RequestStop();
      g_stop_inside_extern_c();
    });
    CHECK(Joined(c, thread, 1000ms));
  });
  CHECK(g_extern_c_unwound.load());
}

REX_TEST_CASE("A stop wakes an IO completion wait with no timeout", "[kernel][shutdown]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 port_ptr = c.Alloc(4);
    CHECKED_ELSE(c.Kernel("NtCreateIoCompletion", {port_ptr, 0, 0, 0}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 port = c.Load32(port_ptr);
    const u32 thread = StopsWithin(c, [port](Caller& t) {
      const u32 out = t.Alloc(16);
      t.Kernel("NtRemoveIoCompletion", {port, out, out + 4, out + 8, 0});
    });
    CHECK(Joined(c, thread, kStopBudget));
  });
}

struct CallerGate {
  std::mutex mutex;
  std::condition_variable changed;
  int arrived = 0;
  bool open = false;
  std::atomic<bool> timed_out{false};

  void ArriveAndWait(int expected) {
    std::unique_lock lock(mutex);
    ++arrived;
    changed.notify_all();
    if (!changed.wait_for(lock, 20s, [&] { return open && arrived >= expected; })) {
      timed_out.store(true);
    }
  }

  void Open() {
    std::lock_guard lock(mutex);
    open = true;
    changed.notify_all();
  }
};

std::string DescribeThread(rex::Runtime& runtime, u32 handle) {
  auto pin = runtime.GetThreadRegistry().FindByHandle(handle);
  if (!pin) {
    return std::format("thread {:08X} is gone", handle);
  }
  return std::format("thread {:08X} '{}' running {} suspend count {} terminated {}", handle,
                     pin->GetName(), pin->IsRunning(), pin->SuspendCount(),
                     pin->GetGuestObject<rex::runtime::X_KTHREAD>()->terminated);
}

REX_TEST_CASE("2 guest threads that call TerminateTitle at once both end", "[kernel][shutdown]") {
  KernelFixture fixture;
  auto gate = std::make_shared<CallerGate>();
  std::vector<u32> callers;
  fixture.RunOnGuestThread([&callers, gate](Caller& c) {
    for (int i = 0; i < 2; ++i) {
      callers.push_back(c.Spawn([gate](Caller& t) {
        gate->ArriveAndWait(2);
        t.GetRuntime().TerminateTitle();
      }));
    }
  });
  gate->Open();
  for (const u32 thread : callers) {
    CHECKED_ELSE(StoppedWithin(fixture.GetRuntime(), thread, 3000ms)) {
      FAIL_CHECK(DescribeThread(fixture.GetRuntime(), thread));
    }
  }
  CHECK_FALSE(gate->timed_out.load());
}

REX_TEST_CASE("TerminateTitle ends a thread suspended after the stop request",
              "[kernel][shutdown][forced_stop]") {
  KernelFixture fixture;
  fixture.GetRuntime().GetFunctionDispatcher()->AddRecompiledCode(REX_RECOMPILED_CODE());
  auto started = std::make_shared<std::atomic<bool>>(false);
  auto unwound = std::make_shared<std::atomic<bool>>(false);
  u32 thread = 0;
  fixture.RunOnGuestThread([&thread, started, unwound](Caller& c) {
    thread = c.Spawn([started, unwound](Caller&) {
      SetOnDestroy guard{unwound};
      started->store(true);
      g_spin_in_generated_code(&g_never_released);
    });
  });
  WaitUntil(*started);
  auto pin = fixture.GetRuntime().GetThreadRegistry().FindByHandle(thread);
  REQUIRE(pin);
  std::thread terminator([&fixture] { fixture.GetRuntime().TerminateTitle(); });
  std::this_thread::sleep_for(100ms);
  u32 previous = 0;
  pin->Suspend(&previous);
  terminator.join();
  CHECK(StoppedWithin(fixture.GetRuntime(), thread, 100ms));
  CHECK(unwound->load());
}

REX_TEST_CASE("Teardown stops a thread whose exit notification waits forever",
              "[kernel][shutdown]") {
  auto fixture = std::make_unique<KernelFixture>();
  const u32 routine =
      fixture->GetRuntime().GetFunctionDispatcher()->AllocateThunk(&WaitingExitRoutine, 0);
  REQUIRE(routine != 0);
  fixture->RunOnGuestThread([routine](Caller& c) {
    g_notify_event.store(CreateEvent(c));
    RegisterThreadNotification(c, routine);
    const u32 event = g_notify_event.load();
    c.Spawn([event](Caller& t) { WaitForever(t, event, 0); });
  });
  std::this_thread::sleep_for(50ms);
  g_exit_wait_armed.store(true);
  const auto start = std::chrono::steady_clock::now();
  fixture.reset();
  g_exit_wait_armed.store(false);
  CHECK(MillisSince(start) < 5000);
}

u32 SocketAddress(Caller& c, u32 address, u32 port) {
  const u32 name = c.Alloc(16);
  c.Store32(name, 0x00020000 | port);
  c.Store32(name + 4, address);
  return name;
}

u32 OpenSocket(Caller& c, u32 type, u32 protocol, bool bound, u32 port = 0) {
  CHECKED_ELSE(c.XAM("NetDll_WSAStartup", {0, 0x0202, c.Alloc(0x200)}) == 0) {
    FAIL_CHECK("the check above failed");
    return 0;
  }
  const u32 sock = c.XAM("NetDll_socket", {0, 2, type, protocol});
  CHECKED_ELSE(sock != 0xFFFFFFFF) {
    FAIL_CHECK("the check above failed");
    return 0;
  }
  if (bound) {
    CHECKED_ELSE(c.XAM("NetDll_bind", {0, sock, SocketAddress(c, 0x7F000001, port), 16}) == 0) {
      FAIL_CHECK("the check above failed");
      return 0;
    }
  }
  return sock;
}

void CheckTerminateWakesSocketCall(const std::function<u32(Caller&)>& open,
                                   const std::function<void(Caller&, u32)>& call) {
  KernelFixture fixture;
  u32 thread = 0;
  fixture.RunOnGuestThread([&thread, open, call](Caller& c) {
    const u32 sock = open(c);
    if (sock == 0) {
      return;
    }
    thread = c.Spawn([sock, call](Caller& t) { call(t, sock); });
    std::this_thread::sleep_for(50ms);
  });
  REQUIRE(thread != 0);
  const auto start = std::chrono::steady_clock::now();
  fixture.GetRuntime().TerminateTitle();
  CHECK(MillisSince(start) < 1000);
  CHECK(StoppedWithin(fixture.GetRuntime(), thread, 100ms));
}

void ReceiveFrom(Caller& t, u32 sock) {
  const u32 buffer = t.Alloc(64);
  t.XAM("NetDll_recvfrom", {0, sock, buffer, 64, 0, 0, 0});
}

REX_TEST_CASE("TerminateTitle wakes a guest thread blocked in a socket receive",
              "[kernel][shutdown]") {
  CheckTerminateWakesSocketCall([](Caller& c) { return OpenSocket(c, 2, 17, true); }, &ReceiveFrom);
}

REX_TEST_CASE("TerminateTitle wakes a guest thread receiving on an unbound socket",
              "[kernel][shutdown]") {
  CheckTerminateWakesSocketCall([](Caller& c) { return OpenSocket(c, 2, 17, false); },
                                &ReceiveFrom);
}

REX_TEST_CASE("TerminateTitle wakes a guest thread blocked in a socket accept",
              "[kernel][shutdown]") {
  CheckTerminateWakesSocketCall(
      [](Caller& c) {
        const u32 sock = OpenSocket(c, 1, 6, true);
        if (sock != 0) {
          CHECK(c.XAM("NetDll_listen", {0, sock, 1}) == 0);
        }
        return sock;
      },
      [](Caller& t, u32 sock) {
        const u32 address = t.Alloc(16);
        const u32 length = t.Alloc(4);
        t.Store32(length, 16);
        t.XAM("NetDll_accept", {0, sock, address, length});
      });
}

REX_TEST_CASE("TerminateTitle wakes a guest thread blocked in a socket select",
              "[kernel][shutdown]") {
  CheckTerminateWakesSocketCall([](Caller& c) { return OpenSocket(c, 2, 17, true); },
                                [](Caller& t, u32 sock) {
                                  const u32 readable = t.Alloc(4 + 64 * 4);
                                  t.Store32(readable, 1);
                                  t.Store32(readable + 4, sock);
                                  t.XAM("NetDll_select", {0, 0, readable, 0, 0, 0});
                                });
}

REX_TEST_CASE("TerminateTitle wakes a guest thread blocked in a send on a full connection",
              "[kernel][shutdown]") {
  CheckTerminateWakesSocketCall(
      [](Caller& c) {
        const u32 listener = OpenSocket(c, 1, 6, true, 47311);
        if (listener == 0) {
          return 0u;
        }
        CHECK(c.XAM("NetDll_listen", {0, listener, 1}) == 0);
        const u32 sock = OpenSocket(c, 1, 6, false);
        if (sock == 0) {
          return 0u;
        }
        CHECKED_ELSE(c.XAM("NetDll_connect", {0, sock, SocketAddress(c, 0x7F000001, 47311), 16}) ==
                     0) {
          FAIL_CHECK("the check above failed");
          return 0u;
        }
        return sock;
      },
      [](Caller& t, u32 sock) {
        constexpr u32 kChunk = 256 * 1024;
        const u32 buffer = t.Alloc(kChunk);
        while (t.XAM("NetDll_send", {0, sock, buffer, kChunk, 0}) != 0xFFFFFFFF) {}
      });
}

REX_TEST_CASE("TerminateTitle wakes a guest thread blocked in a connect", "[kernel][shutdown]") {
  CheckTerminateWakesSocketCall([](Caller& c) { return OpenSocket(c, 1, 6, false); },
                                [](Caller& t, u32 sock) {
                                  t.XAM("NetDll_connect",
                                        {0, sock, SocketAddress(t, 0x0AFFFF01, 80), 16});
                                });
}

REX_TEST_CASE("A receive on a socket the title made nonblocking returns at once",
              "[kernel][shutdown]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 sock = OpenSocket(c, 2, 17, true);
    if (sock == 0) {
      return;
    }
    const u32 enable = c.Alloc(4);
    c.Store32(enable, 1);
    CHECK(c.XAM("NetDll_ioctlsocket", {0, sock, 0x8004667E, enable}) == 0);
    const u32 buffer = c.Alloc(64);
    const auto start = std::chrono::steady_clock::now();
    CHECK(c.XAM("NetDll_recv", {0, sock, buffer, 64, 0}) == 0xFFFFFFFF);
    CHECK(MillisSince(start) < 100);
  });
}

REX_TEST_CASE("A blocking receive gets data sent after it blocked", "[kernel][shutdown]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 receiver = OpenSocket(c, 2, 17, true, 47312);
    const u32 sender = OpenSocket(c, 2, 17, false);
    if (receiver == 0 || sender == 0) {
      return;
    }
    auto received = std::make_shared<std::atomic<u32>>(0);
    const u32 thread = c.Spawn([receiver, received](Caller& t) {
      const u32 buffer = t.Alloc(64);
      received->store(t.XAM("NetDll_recvfrom", {0, receiver, buffer, 64, 0, 0, 0}));
    });
    CHECK(thread != 0);
    std::this_thread::sleep_for(50ms);
    const u32 payload = c.Alloc(4);
    c.Store32(payload, 0x52455821);
    CHECK(c.XAM("NetDll_sendto",
                {0, sender, payload, 4, 0, SocketAddress(c, 0x7F000001, 47312), 16}) == 4);
    const auto start = std::chrono::steady_clock::now();
    while (received->load() == 0 && MillisSince(start) < 1000) {
      std::this_thread::sleep_for(1ms);
    }
    CHECK(received->load() == 4);
  });
}

REX_TEST_CASE("A send to a peer that closed returns an error", "[kernel][socket]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 listener = OpenSocket(c, 1, 6, true, 47313);
    const u32 sock = OpenSocket(c, 1, 6, false);
    if (listener == 0 || sock == 0) {
      return;
    }
    CHECK(c.XAM("NetDll_listen", {0, listener, 1}) == 0);
    CHECKED_ELSE(c.XAM("NetDll_connect", {0, sock, SocketAddress(c, 0x7F000001, 47313), 16}) == 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 address = c.Alloc(16);
    const u32 length = c.Alloc(4);
    c.Store32(length, 16);
    const u32 peer = c.XAM("NetDll_accept", {0, listener, address, length});
    CHECKED_ELSE(peer != 0xFFFFFFFF) {
      FAIL_CHECK("the check above failed");
      return;
    }
    CHECK(c.Load32(address) >> 16 == 2);
    CHECK(c.Load32(address + 4) == 0x7F000001);
    CHECK(c.Load32(length) == 16);
    CHECK(c.XAM("NetDll_closesocket", {0, peer}) == 0);
    const u32 payload = c.Alloc(4);
    u32 result = 0;
    for (int i = 0; i < 100 && result != 0xFFFFFFFF; ++i) {
      result = c.XAM("NetDll_send", {0, sock, payload, 4, 0});
      std::this_thread::sleep_for(10ms);
    }
    CHECK(result == 0xFFFFFFFF);
  });
}

REX_TEST_CASE("A datagram sent with sendto arrives with its source address", "[kernel][socket]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 receiver = OpenSocket(c, 2, 17, true, 47314);
    const u32 sender = OpenSocket(c, 2, 17, true, 47315);
    if (receiver == 0 || sender == 0) {
      return;
    }
    const u32 payload = c.Alloc(4);
    c.Store32(payload, 0x52455821);
    CHECK(c.XAM("NetDll_sendto",
                {0, sender, payload, 4, 0, SocketAddress(c, 0x7F000001, 47314), 16}) == 4);
    const u32 enable = c.Alloc(4);
    c.Store32(enable, 1);
    CHECK(c.XAM("NetDll_ioctlsocket", {0, receiver, 0x8004667E, enable}) == 0);
    const u32 buffer = c.Alloc(64);
    const u32 from = c.Alloc(16);
    const u32 from_length = c.Alloc(4);
    u32 received = 0xFFFFFFFF;
    const auto start = std::chrono::steady_clock::now();
    while (received == 0xFFFFFFFF && MillisSince(start) < 1000) {
      c.Store32(from_length, 16);
      received = c.XAM("NetDll_recvfrom", {0, receiver, buffer, 64, 0, from, from_length});
      std::this_thread::sleep_for(1ms);
    }
    CHECK(received == 4);
    CHECK(c.Load32(buffer) == 0x52455821);
    CHECK(c.Load32(from) == (0x00020000 | 47315));
    CHECK(c.Load32(from + 4) == 0x7F000001);
  });
}

#if !REX_PLATFORM_WIN32
REX_TEST_CASE("A stop that crosses an SEH scope leaves no scope open on the thread",
              "[kernel][shutdown][seh]") {
  KernelFixture fixture;
  const u32 routine =
      fixture.GetRuntime().GetFunctionDispatcher()->AllocateThunk(&ProbeScopeOnExit, 0);
  REQUIRE(routine != 0);
  g_scope_probe_ran.store(false);
  g_scope_left_open.store(false);
  fixture.RunOnGuestThread([routine](Caller& c) {
    RegisterThreadNotification(c, routine);
    const u32 event = CreateEvent(c);
    auto handled = std::make_shared<std::atomic<bool>>(false);
    auto waiting = std::make_shared<std::atomic<bool>>(false);
    g_scope_probe_armed.store(true);
    const u32 thread = c.Spawn(
        [event, handled, waiting](Caller& t) { GuardedWait(t, event, *handled, *waiting); });
    WaitUntil(*waiting);
    RequestStop(c, thread);
    CHECK(Joined(c, thread, kStopBudget));
    g_scope_probe_armed.store(false);
  });
  CHECK(g_scope_probe_ran.load());
  CHECK_FALSE(g_scope_left_open.load());
}

REX_TEST_CASE("A guest fiber starts with no SEH scope and the switch back restores the scope",
              "[kernel][shutdown][seh][fiber]") {
  KernelFixture fixture;
  const u32 fiber_entry =
      fixture.GetRuntime().GetFunctionDispatcher()->AllocateThunk(&ProbeScopeOnFiber, 0);
  REQUIRE(fiber_entry != 0);
  g_fiber_probed.store(false);
  g_fiber_saw_scope.store(true);
  auto kept = std::make_shared<std::atomic<bool>>(false);
  fixture.RunOnGuestThread([fiber_entry, kept](Caller& c) {
    const u32 thread = c.Spawn(
        [fiber_entry, kept](Caller& t) { kept->store(SwitchToFiberInScope(t, fiber_entry)); });
    CHECK(Joined(c, thread, 1000ms));
  });
  CHECK(g_fiber_probed.load());
  CHECK_FALSE(g_fiber_saw_scope.load());
  CHECK(kept->load());
}
#endif

REX_TEST_CASE("A suspend never parks a guest thread inside a host allocation", "[kernel][thread]") {
  KernelFixture fixture;
  auto stop = std::make_shared<std::atomic<bool>>(false);
  auto allocating = std::make_shared<std::atomic<bool>>(false);
  u32 target = 0;
  fixture.RunOnGuestThread([&target, stop, allocating](Caller& c) {
    target = c.Spawn([stop, allocating](Caller&) {
      allocating->store(true);
      while (!stop->load()) {
        auto block = std::make_unique<std::array<u8, 256>>();
        g_allocation_sink.store(block.get());
      }
    });
  });
  WaitUntil(*allocating);
  auto pin = fixture.GetRuntime().GetThreadRegistry().FindByHandle(target);
  REQUIRE(pin);
  std::atomic<u32> asked{0};
  std::atomic<u32> answered{0};
  std::thread allocator([&] {
    while (!stop->load()) {
      if (answered.load() < asked.load()) {
        auto block = std::make_unique<std::array<u8, 256>>();
        g_allocation_sink.store(block.get());
        answered.fetch_add(1);
      }
    }
  });
  bool allocated_every_time = true;
  for (u32 i = 1; i <= 200 && allocated_every_time; ++i) {
    u32 previous = 0;
    pin->Suspend(&previous);
    asked.store(i);
    const auto until = std::chrono::steady_clock::now() + 1s;
    while (answered.load() < i && std::chrono::steady_clock::now() < until) {
      std::this_thread::yield();
    }
    allocated_every_time = answered.load() == i;
    pin->Resume(&previous);
  }
  stop->store(true);
  allocator.join();
  CHECK(allocated_every_time);
}

REX_TEST_CASE("Setup fails when kernel initialization fails", "[kernel][kernel_init]") {
  rex::Config config;
  config.modules = {"rexfilesystem", "rexruntime"};
  auto api = rex::Init(config);
  REQUIRE(api);
  {
    rex::Runtime runtime((*api)->GetConfig());
    rex::RuntimeDesc setup;
    setup.file_system = (*api)->Get<rex::filesystem::IFileSystem>();
    setup.kernel_init = [](rex::Runtime&) -> rex::Result<void> {
      return rex::Err(rex::ErrorCategory::kRuntime, "kernel module did not load");
    };
    setup.tool_mode = true;
    const rex::Result<void> result = runtime.Setup(setup);
    CHECK_FALSE(result);
    if (!result) {
      CHECK(result.error().message.find("kernel module did not load") != std::string::npos);
    }
    runtime.Shutdown();
  }
  rex::Shutdown();
}
