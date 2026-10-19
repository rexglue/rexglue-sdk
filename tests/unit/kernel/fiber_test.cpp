/**
 * @file        tests/unit/kernel/fiber_test.cpp
 * @brief       Fiber switches through the shim fiber routines
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>
#include <cstring>

#include <rex/platform/lock_order.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/thread_registry.h>
#include <rex/runtime/xmemory.h>

#include "kernel_fixture.h"
#include "runtime/guest_stack.h"

extern "C" void rexshim_ConvertThreadToFiber(PPCContext& ctx, rex::u8* base);
extern "C" void rexshim_ConvertFiberToThread(PPCContext& ctx, rex::u8* base);
extern "C" void rexshim_CreateFiber(PPCContext& ctx, rex::u8* base);
extern "C" void rexshim_DeleteFiber(PPCContext& ctx, rex::u8* base);
extern "C" void rexshim_SwitchToFiber(PPCContext& ctx, rex::u8* base);

using rex::u32;
using rex::u64;
using rex::u8;
using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {

std::atomic<int> g_violations{0};

void CountViolation(rex::platform::LockRank, rex::platform::LockRank) {
  ++g_violations;
}

constexpr PPCRegister PPCContext::* kGPRs[] = {
    &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18,
    &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28,
    &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};

constexpr PPCRegister PPCContext::* kFPRs[] = {
    &PPCContext::f14, &PPCContext::f15, &PPCContext::f16, &PPCContext::f17, &PPCContext::f18,
    &PPCContext::f19, &PPCContext::f20, &PPCContext::f21, &PPCContext::f22, &PPCContext::f23,
    &PPCContext::f24, &PPCContext::f25, &PPCContext::f26, &PPCContext::f27, &PPCContext::f28,
    &PPCContext::f29, &PPCContext::f30, &PPCContext::f31};

constexpr PPCVRegister PPCContext::* kVRs[] = {&PPCContext::v14, &PPCContext::v31, &PPCContext::v64,
                                               &PPCContext::v95, &PPCContext::v127};

u32 g_main_fiber = 0;
std::atomic<u32> g_fiber_kthread{0};

void Fill(PPCContext& ctx, u64 gpr, rex::f64 fpr, u8 vr) {
  for (const auto reg : kGPRs) {
    (ctx.*reg).u64 = gpr;
  }
  for (const auto reg : kFPRs) {
    (ctx.*reg).f64 = fpr;
  }
  for (const auto reg : kVRs) {
    std::memset(&(ctx.*reg), vr, sizeof(PPCVRegister));
  }
}

void FiberBody(PPCContext& ctx, u8* base) {
  g_fiber_kthread = Caller(ctx, base, rex::runtime::GetCurrent()).Load32(ctx.r13.u32 + 0x100);
  Fill(ctx, 0xBAD, -1.0, 0xEE);
  ctx.SetCR(0x12345678);
  ctx.lr = 0x8200BEEF;
  ctx.fpscr.StoreFromGuest(rex::ppc::kRoundUp);
  ctx.r3.u64 = g_main_fiber;
  rexshim_SwitchToFiber(ctx, base);
}

}  // namespace

REX_TEST_CASE("fibers: a switch away and back keeps the nonvolatile state", "[kernel][fibers]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    PPCContext& ctx = c.GetCtx();
    auto* memory = c.GetRuntime().GetMemory();
    u8* base = memory->GetVirtualMembase();
    ctx.r3.u64 = 0;
    rexshim_ConvertThreadToFiber(ctx, base);
    g_main_fiber = ctx.r3.u32;
    CHECKED_ELSE(g_main_fiber != 0) {
      FAIL_CHECK("the check above failed");
      return;
    }

    const u32 body = c.GetRuntime().GetFunctionDispatcher()->AllocateThunk(&FiberBody, 0);
    CHECKED_ELSE(body != 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 used = memory->GetTitlePages();
    ctx.r3.u64 = 0;
    ctx.r4.u64 = body;
    ctx.r5.u64 = 0;
    rexshim_CreateFiber(ctx, base);
    const u32 fiber = ctx.r3.u32;
    CHECKED_ELSE(fiber != 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    CHECK(memory->GetTitlePages() == used + 16);

    Fill(ctx, 0x1234, 3.25, 0x5A);
    ctx.SetCR(0x89ABCDEF);
    ctx.lr = 0x82001234;
    ctx.fpscr.StoreFromGuest(rex::ppc::kRoundDown);
    ctx.r3.u64 = fiber;
    rexshim_SwitchToFiber(ctx, base);

    for (const auto reg : kGPRs) {
      CHECK((ctx.*reg).u64 == 0x1234);
    }
    for (const auto reg : kFPRs) {
      CHECK((ctx.*reg).f64 == 3.25);
    }
    for (const auto reg : kVRs) {
      CHECK((ctx.*reg).u64[0] == 0x5A5A5A5A5A5A5A5Aull);
      CHECK((ctx.*reg).u64[1] == 0x5A5A5A5A5A5A5A5Aull);
    }
    CHECK(ctx.GetCR() == 0x89ABCDEF);
    CHECK(ctx.lr == 0x82001234);
    CHECK(ctx.fpscr.LoadFromHost() == rex::ppc::kRoundDown);
    CHECK(g_fiber_kthread == c.Load32(ctx.r13.u32 + 0x100));

    ctx.r3.u64 = fiber;
    rexshim_DeleteFiber(ctx, base);
    CHECK(memory->GetTitlePages() == used);
    rexshim_ConvertFiberToThread(ctx, base);
    ctx.fpscr.StoreFromGuest(rex::ppc::kRoundNearest);
  });
}

REX_TEST_CASE("fibers: ResolveGuestStackSize allows 12 KB fiber stacks", "[kernel][fibers]") {
  CHECK(rex::runtime::ResolveGuestStackSize(0x2000, 0x10000, 12 * 1024) == 0x3000);
  CHECK(rex::runtime::ResolveGuestStackSize(0, 0x10000, 12 * 1024) == 0x10000);
  CHECK(rex::runtime::ResolveGuestStackSize(0x3001, 0x10000, 12 * 1024) == 0x4000);
}

REX_TEST_CASE("Teardown frees a fiber context in a physical heap without breaking the lock order",
              "[kernel][fibers][lock_order]") {
#ifdef NDEBUG
  SKIP("lock order is checked in debug builds only");
#endif
  g_violations = 0;
  rex::platform::LockOrderHandler previous = nullptr;
  {
    KernelFixture fixture;
    fixture.RunOnGuestThread([](Caller& c) {
      const u32 context = c.GetRuntime().GetMemory()->SystemHeapAlloc(
          0x1000, 0x1000, rex::runtime::kSystemHeapPhysical);
      REQUIRE(context != 0);
      c.GetRuntime().GetThreadRegistry().RegisterFiber(
          context, rex::runtime::FiberInfo{nullptr, 0, context, 0, true});
    });
    previous = rex::platform::SetLockOrderHandler(&CountViolation);
  }
  rex::platform::SetLockOrderHandler(previous);
  CHECK(g_violations == 0);
}
