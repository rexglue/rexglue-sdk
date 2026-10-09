/**
 * @file        tests/unit/kernel/char_dpc_test.cpp
 * @brief       DPC dispatch against the XDK rules
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <rex/platform/lock_order.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xmemory.h>

#include "kernel_fixture.h"

using rex::u32;
using rex::u64;
using rex::u8;
using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {

constexpr u32 kDPCStackBegin = 0x70000000;
constexpr u32 kDPCStackEnd = 0x7F000000;

std::mutex g_mutex;
std::vector<u32> g_order;
std::atomic<int> g_runs{0};
std::atomic<int> g_active{0};
std::atomic<int> g_max_active{0};
std::atomic<u32> g_dpc_r1{0};
std::atomic<u32> g_thread_stack_base{0};
std::atomic<u32> g_thread_stack_end{0};
std::atomic<u32> g_irql_in_dpc{0xFF};
std::atomic<u32> g_chained_dpc{0};
std::atomic<int> g_ran_inside_isr{-1};
std::atomic<u32> g_isr_dpc{0};
u32 g_safe_event = 0;
u32 g_safe_semaphore = 0;
u32 g_safe_mutant = 0;
std::vector<u32> g_safe_results;

Caller CallerFor(PPCContext& ctx, u8* base) {
  return Caller(ctx, base, rex::runtime::Current());
}

void Record(u32 tag) {
  std::lock_guard lock(g_mutex);
  g_order.push_back(tag);
}

void CountingDPC(PPCContext& ctx, u8*) {
  ++g_runs;
  Record(ctx.r4.u32);
  ctx.r3.u64 = 0;
}

void OverlapDPC(PPCContext& ctx, u8*) {
  const int now = ++g_active;
  int expected = g_max_active.load();
  while (now > expected && !g_max_active.compare_exchange_weak(expected, now)) {}
  std::this_thread::sleep_for(std::chrono::microseconds(200));
  --g_active;
  ++g_runs;
  ctx.r3.u64 = 0;
}

void StackDPC(PPCContext& ctx, u8* base) {
  Caller caller = CallerFor(ctx, base);
  g_dpc_r1 = ctx.r1.u32;
  g_thread_stack_base = caller.Load32(ctx.r13.u32 + 0x70);
  g_thread_stack_end = caller.Load32(ctx.r13.u32 + 0x74);
  g_irql_in_dpc = *rex::runtime::Current().memory()->TranslateVirtual<u8*>(ctx.r13.u32 + 0x18);
  ++g_runs;
  ctx.r3.u64 = 0;
}

void ChainingDPC(PPCContext& ctx, u8* base) {
  Caller caller = CallerFor(ctx, base);
  Record(1);
  caller.Kernel("KeInsertQueueDpc", {g_chained_dpc.load(), 0, 0});
  Record(2);
  ctx.r3.u64 = 0;
}

void ChainedDPC(PPCContext& ctx, u8*) {
  Record(3);
  ctx.r3.u64 = 0;
}

std::atomic<u32> g_self_dpc{0};

void RequeueingDPC(PPCContext& ctx, u8* base) {
  const int now = ++g_active;
  int expected = g_max_active.load();
  while (now > expected && !g_max_active.compare_exchange_weak(expected, now)) {}
  if (++g_runs == 1) {
    Caller caller = CallerFor(ctx, base);
    Record(caller.Kernel("KeInsertQueueDpc", {g_self_dpc.load(), 0, 0}));
    Record(caller.Kernel("KeInsertQueueDpc", {g_self_dpc.load(), 0, 0}));
  }
  --g_active;
  ctx.r3.u64 = 0;
}

void QueueingISR(PPCContext& ctx, u8* base) {
  Caller caller = CallerFor(ctx, base);
  caller.Kernel("KeInsertQueueDpc", {g_isr_dpc.load(), 0, 0});
  g_ran_inside_isr = g_runs.load();
  ctx.r3.u64 = 0;
}

void SafeCallDPC(PPCContext& ctx, u8* base) {
  Caller caller = CallerFor(ctx, base);
  std::vector<u32> results;
  results.push_back(caller.Kernel("KeSetEvent", {g_safe_event, 0, 0}));
  results.push_back(caller.Kernel("KeResetEvent", {g_safe_event}));
  results.push_back(caller.Kernel("KePulseEvent", {g_safe_event, 0, 0}));
  results.push_back(caller.Kernel("KeReleaseSemaphore", {g_safe_semaphore, 0, 1, 0}));
  results.push_back(caller.Kernel("KeReleaseMutant", {g_safe_mutant, 0, 0, 0}));
  std::lock_guard lock(g_mutex);
  g_safe_results = results;
  ++g_runs;
  ctx.r3.u64 = 0;
}

void Reset() {
  std::lock_guard lock(g_mutex);
  g_order.clear();
  g_runs = 0;
  g_active = 0;
  g_max_active = 0;
  g_ran_inside_isr = -1;
  g_safe_results.clear();
}

u32 MakeDPC(Caller& c, void (*routine)(PPCContext&, u8*), u32 context = 0) {
  const u32 thunk = c.runtime().function_dispatcher()->AllocateThunk(routine, 0);
  CHECKED_ELSE(thunk != 0) {
    FAIL_CHECK("the check above failed");
    return 0;
  }
  const u32 dpc = c.Alloc(0x20);
  c.Kernel("KeInitializeDpc", {dpc, thunk, context});
  return dpc;
}

std::atomic<int> g_violations{0};

void CountViolation(rex::platform::LockRank, rex::platform::LockRank) {
  ++g_violations;
}

std::pair<u32, u32> IgnoreInvalidation(void*, u32, u32, bool) {
  return {0, UINT32_MAX};
}

std::vector<u32> Order() {
  std::lock_guard lock(g_mutex);
  return g_order;
}

}  // namespace

TEST_CASE("A DPC queued by an ISR runs after the ISR returns", "[kernel][dpc]") {
  Reset();
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    g_isr_dpc = MakeDPC(c, &CountingDPC);
    auto* dispatcher = c.runtime().function_dispatcher();
    const u32 isr = dispatcher->AllocateThunk(&QueueingISR, 0);
    const u32 pcr = c.Alloc(0x1000);
    const u32 stack = c.Alloc(0x10000) + 0x8000;
    std::thread interrupt([&] {
      rex::runtime::ThreadState state(5, stack, pcr, c.runtime().memory(), &c.runtime());
      dispatcher->ExecuteInterrupt(&state, isr, nullptr, 0);
    });
    interrupt.join();
  });
  CHECK(g_ran_inside_isr == 0);
  CHECK(g_runs == 1);
}

TEST_CASE("A DPC inserted at passive level runs before KeInsertQueueDpc returns", "[kernel][dpc]") {
  Reset();
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 dpc = MakeDPC(c, &CountingDPC, 7);
    CHECK(c.Kernel("KeInsertQueueDpc", {dpc, 1, 2}) == 1);
    CHECK(g_runs == 1);
    CHECK(Order() == std::vector<u32>{7});
  });
}

TEST_CASE("A DPC inserted at raised IRQL runs when KfLowerIrql lowers it", "[kernel][dpc]") {
  Reset();
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 dpc = MakeDPC(c, &CountingDPC);
    const u32 old_irql = c.Kernel("KeRaiseIrqlToDpcLevel");
    CHECK(c.Kernel("KeInsertQueueDpc", {dpc, 0, 0}) == 1);
    CHECK(c.Kernel("KeInsertQueueDpc", {dpc, 0, 0}) == 0);
    CHECK(g_runs == 0);
    c.Kernel("KfLowerIrql", {old_irql});
    CHECK(g_runs == 1);
  });
}

TEST_CASE("A DPC queued from a DPC runs after it", "[kernel][dpc]") {
  Reset();
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    g_chained_dpc = MakeDPC(c, &ChainedDPC);
    const u32 first = MakeDPC(c, &ChainingDPC);
    CHECK(c.Kernel("KeInsertQueueDpc", {first, 0, 0}) == 1);
    CHECK(Order() == std::vector<u32>{1, 2, 3});
  });
}

TEST_CASE("A DPC that queues itself runs again once after it returns", "[kernel][dpc]") {
  Reset();
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    g_self_dpc = MakeDPC(c, &RequeueingDPC);
    CHECK(c.Kernel("KeInsertQueueDpc", {g_self_dpc.load(), 0, 0}) == 1);
  });
  CHECK(g_runs == 2);
  CHECK(g_max_active == 1);
  CHECK(Order() == std::vector<u32>{1, 0});
}

TEST_CASE("DPCs inserted from 2 threads never overlap", "[kernel][dpc]") {
  Reset();
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto worker = [](Caller& o) {
      const u32 dpc = MakeDPC(o, &OverlapDPC);
      for (int i = 0; i < 200; ++i) {
        o.Kernel("KeInsertQueueDpc", {dpc, 0, 0});
      }
    };
    const u32 first = c.Spawn(worker);
    const u32 second = c.Spawn(worker);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {first, 1, 0, c.Quad(-100000000)}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {second, 1, 0, c.Quad(-100000000)}) == 0);
    c.Kernel("NtClose", {first});
    c.Kernel("NtClose", {second});
  });
  CHECK(g_max_active == 1);
  CHECK(g_runs > 0);
}

TEST_CASE("KeRemoveQueueDpc removes a queued DPC once", "[kernel][dpc]") {
  Reset();
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 dpc = MakeDPC(c, &CountingDPC);
    const u32 old_irql = c.Kernel("KeRaiseIrqlToDpcLevel");
    CHECK(c.Kernel("KeInsertQueueDpc", {dpc, 0, 0}) == 1);
    CHECK(c.Kernel("KeRemoveQueueDpc", {dpc}) == 1);
    CHECK(c.Kernel("KeRemoveQueueDpc", {dpc}) == 0);
    c.Kernel("KfLowerIrql", {old_irql});
    CHECK(g_runs == 0);
  });
}

TEST_CASE("KeRetireDpcList runs the queue", "[kernel][dpc]") {
  Reset();
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 dpc = MakeDPC(c, &CountingDPC);
    const u32 old_irql = c.Kernel("KeRaiseIrqlToDpcLevel");
    CHECK(c.Kernel("KeInsertQueueDpc", {dpc, 0, 0}) == 1);
    c.Kernel("KeRetireDpcList");
    CHECK(g_runs == 1);
    c.Kernel("KfLowerIrql", {old_irql});
    CHECK(g_runs == 1);
  });
}

TEST_CASE("A DPC runs on the DPC stack at DISPATCH_LEVEL", "[kernel][dpc]") {
  Reset();
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 dpc = MakeDPC(c, &StackDPC);
    CHECK(c.Kernel("KeInsertQueueDpc", {dpc, 0, 0}) == 1);
  });
  REQUIRE(g_runs == 1);
  const u32 r1 = g_dpc_r1;
  CHECK((r1 < g_thread_stack_end || r1 > g_thread_stack_base));
  CHECK(r1 >= kDPCStackBegin);
  CHECK(r1 < kDPCStackEnd);
  CHECK(g_irql_in_dpc == 2);
}

TEST_CASE("The DPC safe calls work inside a DPC", "[kernel][dpc]") {
  Reset();
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    g_safe_event = c.Alloc(0x10);
    c.Kernel("KeInitializeEvent", {g_safe_event, 0, 0});
    g_safe_semaphore = c.Alloc(0x14);
    c.Kernel("KeInitializeSemaphore", {g_safe_semaphore, 0, 10});
    g_safe_mutant = c.Alloc(0x20);
    c.Kernel("KeInitializeMutant", {g_safe_mutant, 1});
    const u32 dpc = MakeDPC(c, &SafeCallDPC);
    CHECK(c.Kernel("KeInsertQueueDpc", {dpc, 0, 0}) == 1);
  });
  REQUIRE(g_runs == 1);
  std::lock_guard lock(g_mutex);
  REQUIRE(g_safe_results.size() == 5);
  CHECK(g_safe_results[3] == 0);
  CHECK(g_safe_results[4] == 0);
}

TEST_CASE("Queueing a DPC that sits in a write watched page keeps the lock order",
          "[kernel][dpc][lock_order]") {
#ifdef NDEBUG
  SKIP("lock order is checked in debug builds only");
#endif
  Reset();
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto& memory = *c.runtime().memory();
    const u32 thunk = c.runtime().function_dispatcher()->AllocateThunk(&CountingDPC, 0);
    const u32 dpc = memory.SystemHeapAlloc(0x1000, 0x1000, rex::runtime::kSystemHeapPhysical);
    REQUIRE(dpc != 0);
    c.Kernel("KeInitializeDpc", {dpc, thunk, 7});
    void* handle = memory.RegisterPhysicalMemoryInvalidationCallback(IgnoreInvalidation, nullptr);
    memory.EnablePhysicalMemoryAccessCallbacks(memory.GetPhysicalAddress(dpc), 0x1000, true, false);
    g_violations = 0;
    const auto previous = rex::platform::SetLockOrderHandler(&CountViolation);
    CHECK(c.Kernel("KeInsertQueueDpc", {dpc, 1, 2}) == 1);
    rex::platform::SetLockOrderHandler(previous);
    memory.UnregisterPhysicalMemoryInvalidationCallback(handle);
    CHECK(g_violations == 0);
    CHECK(g_runs == 1);
  });
}
