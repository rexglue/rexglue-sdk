/**
 * @file        tests/unit/kernel/interrupt_test.cpp
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
#include <thread>

#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime.h>
#include <rex/runtime/thread_state.h>

#include "kernel_fixture.h"
#include "runtime/dispatcher.h"

using rex::test::Caller;
using rex::test::KernelFixture;

namespace {

std::atomic<int> g_active;
std::atomic<int> g_max;

void TrackOverlap(PPCContext& ctx, u8*) {
  const int now = ++g_active;
  int expected = g_max.load();
  while (now > expected && !g_max.compare_exchange_weak(expected, now)) {}
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  --g_active;
  ctx.r3.u64 = 0;
}

std::atomic<u32> g_isr_irql{0xFF};
std::atomic<u32> g_isr_dpc{0};
u64 g_dpc_r14 = 0;
u64 g_after_r14 = 0;
f64 g_after_f14 = 0.0;

void CaptureDPCRegisters(PPCContext& ctx, u8*) {
  g_dpc_r14 = ctx.r14.u64;
  ctx.r3.u64 = 0;
}

void ClobberingISR(PPCContext& ctx, u8* base) {
  g_isr_irql = *rex::runtime::GetCurrent().GetMemory()->TranslateVirtual<u8*>(ctx.r13.u32 + 0x18);
  ctx.r14.u64 = 0xDEAD;
  ctx.f14.f64 = 7.5;
  Caller(ctx, base, rex::runtime::GetCurrent())
      .Kernel("KeInsertQueueDpc", {g_isr_dpc.load(), 0, 0});
  ctx.r3.u64 = 0;
}

}  // namespace

REX_TEST_CASE("interrupt callbacks never overlap and never block guest threads",
              "[kernel][interrupt]") {
  g_active = 0;
  g_max = 0;
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto* dispatcher = c.GetRuntime().GetFunctionDispatcher();
    auto* memory = c.GetRuntime().GetMemory();
    const u32 callback = dispatcher->AllocateThunk(&TrackOverlap, 0);
    CHECKED_ELSE(callback != 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 pcr_first = c.Alloc(0x1000);
    const u32 pcr_second = c.Alloc(0x1000);
    const u32 stack_first = c.Alloc(0x10000) + 0x8000;
    const u32 stack_second = c.Alloc(0x10000) + 0x8000;
    std::thread first([&] {
      rex::runtime::ThreadState state(1, stack_first, pcr_first, memory, &c.GetRuntime());
      dispatcher->ExecuteInterrupt(&state, callback, nullptr, 0);
    });
    std::thread second([&] {
      rex::runtime::ThreadState state(2, stack_second, pcr_second, memory, &c.GetRuntime());
      dispatcher->ExecuteInterrupt(&state, callback, nullptr, 0);
    });
    for (int i = 0; i < 1000; ++i) {
      const u32 old_irql = c.Kernel("KeRaiseIrqlToDpcLevel");
      c.Kernel("KfLowerIrql", {old_irql});
    }
    first.join();
    second.join();
  });
  CHECK(g_max == 1);
}

REX_TEST_CASE("interrupts leave the guest thread state intact", "[kernel][interrupt]") {
  g_active = 0;
  g_max = 0;
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto* dispatcher = c.GetRuntime().GetFunctionDispatcher();
    const u32 callback = dispatcher->AllocateThunk(&TrackOverlap, 0);
    CHECKED_ELSE(callback != 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 pcr = c.Alloc(0x1000);
    c.Store32(pcr, 0x1234);
    rex::runtime::ThreadState state(3, c.Alloc(0x10000) + 0x8000, pcr, c.GetRuntime().GetMemory(),
                                    &c.GetRuntime());
    CHECK(dispatcher->ExecuteInterrupt(&state, callback, nullptr, 0) == 0);
    CHECK(c.Load32(pcr) == 0x1234);
  });
  CHECK(g_max == 1);
}

REX_TEST_CASE("DPC init and removal from 2 guest threads leaves the list empty",
              "[kernel][interrupt]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto worker = [](Caller& o) {
      const u32 dpc = o.Alloc(0x40);
      for (int i = 0; i < 200; ++i) {
        o.Kernel("KeInitializeDpc", {dpc, 0x82000000, 0});
        o.Kernel("KeRemoveQueueDpc", {dpc});
      }
    };
    const u32 first = c.Spawn(worker);
    const u32 second = c.Spawn(worker);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {first, 1, 0, c.Quad(-100000000)}) == 0);
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {second, 1, 0, c.Quad(-100000000)}) == 0);
    c.Kernel("NtClose", {first});
    c.Kernel("NtClose", {second});
    CHECK_FALSE(
        static_cast<rex::runtime::Dispatcher&>(c.GetRuntime().GetDispatcher()).HasQueuedDPCs());
  });
}

REX_TEST_CASE("An interrupt runs at IRQL 3 and leaves no register change behind",
              "[kernel][interrupt]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto* dispatcher = c.GetRuntime().GetFunctionDispatcher();
    const u32 isr = dispatcher->AllocateThunk(&ClobberingISR, 0);
    const u32 dpc_routine = dispatcher->AllocateThunk(&CaptureDPCRegisters, 0);
    CHECKED_ELSE(isr != 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    CHECKED_ELSE(dpc_routine != 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 dpc = c.Alloc(0x20);
    c.Kernel("KeInitializeDpc", {dpc, dpc_routine, 0});
    g_isr_dpc = dpc;
    const u32 pcr = c.Alloc(0x1000);
    const u32 stack = c.Alloc(0x10000) + 0x8000;
    std::thread interrupt([&] {
      rex::runtime::ThreadState state(7, stack, pcr, c.GetRuntime().GetMemory(), &c.GetRuntime());
      state.GetContext()->r14.u64 = 0x1111;
      state.GetContext()->f14.f64 = 2.5;
      dispatcher->ExecuteInterrupt(&state, isr, nullptr, 0);
      g_after_r14 = state.GetContext()->r14.u64;
      g_after_f14 = state.GetContext()->f14.f64;
    });
    interrupt.join();
    CHECK(c.Load8(pcr + 0x18) == 0);
  });
  CHECK(g_isr_irql == 3);
  CHECK(g_after_r14 == 0x1111);
  CHECK(g_after_f14 == 2.5);
  CHECK(g_dpc_r14 == 0x1111);
}
