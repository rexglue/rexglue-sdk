/**
 * @file        tests/unit/ppc/kernel_hooks_test.cpp
 * @brief       Kernel hook registration
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>

#include <rex/ppc/call.h>
#include <rex/ppc/context.h>
#include <rex/ppc/hook.h>
#include <rex/ppc/kernel_hook.h>
#include <rex/ppc/recompiled_code.h>

namespace {

u32 g_resolved_address = 0;
u32 g_detour_checks = 0;
u32 g_debug_text_writes = 0;
u32 g_debug_text_address = 0;
u32 g_debug_text_length = 0;
u8 g_fake_base = 0;
PPCContext g_fake_ctx;

void FakeGuest(PPCContext&, u8*) {}

rex::ppc::BoundThread FakeGetBoundThread() {
  return {&g_fake_ctx, &g_fake_base};
}

PPCFunc* FakeResolveFunction(u32 guest_address) {
  g_resolved_address = guest_address;
  return &FakeGuest;
}

void FakeCheckBoundDetourArguments(const char*, PPCFunc*, u8, u16) {
  ++g_detour_checks;
}

void FakeWriteDebugText(u32 address, u32 length) {
  ++g_debug_text_writes;
  g_debug_text_address = address;
  g_debug_text_length = length;
}

struct HooksGuard {
  ~HooksGuard() { rex::ppc::SetKernelHooks(nullptr); }
};

}  // namespace

REX_TEST_CASE("The PPC runtime functions reach the kernel through registered hooks",
              "[ppc][kernel_hooks]") {
  HooksGuard guard;
  rex::ppc::SetKernelHooks(nullptr);
  g_resolved_address = 0;
  g_detour_checks = 0;
  g_debug_text_writes = 0;

  const rex::ppc::KernelHooks fake = {
      FakeGetBoundThread, FakeResolveFunction, nullptr, nullptr, FakeCheckBoundDetourArguments,
      FakeWriteDebugText,
  };

  SECTION("Without hooks no thread is bound and an address resolves to a trap") {
    const rex::ppc::BoundThread bound = rex::ppc::GetBoundThread();
    CHECK(bound.ctx == nullptr);
    CHECK(bound.base == nullptr);
    PPCFunc* func = rex::ppc::ResolveFunction(0x82001000);
    CHECK(func != nullptr);
    CHECK(func != &FakeGuest);
    CHECK(g_resolved_address == 0);
    rex::ppc::detail::CheckBoundDetourArguments("Detour", &FakeGuest, 0, 0);
    CHECK(g_detour_checks == 0);
  }

  SECTION("With hooks the calls reach the fake") {
    rex::ppc::SetKernelHooks(&fake);
    const rex::ppc::BoundThread bound = rex::ppc::GetBoundThread();
    CHECK(bound.ctx == &g_fake_ctx);
    CHECK(bound.base == &g_fake_base);
    CHECK(rex::ppc::ResolveFunction(0x82001000) == &FakeGuest);
    CHECK(g_resolved_address == 0x82001000);
    rex::ppc::detail::CheckBoundDetourArguments("Detour", &FakeGuest, 1, 2);
    CHECK(g_detour_checks == 1);
  }

  SECTION("Clearing the hooks restores the behaviour without a kernel") {
    rex::ppc::SetKernelHooks(&fake);
    rex::ppc::SetKernelHooks(nullptr);
    CHECK(rex::ppc::GetBoundThread().ctx == nullptr);
    CHECK(rex::ppc::ResolveFunction(0x82002000) != &FakeGuest);
    CHECK(g_resolved_address == 0);
    rex::ppc::detail::CheckBoundDetourArguments("Detour", &FakeGuest, 0, 0);
    CHECK(g_detour_checks == 0);
  }
}

REX_TEST_CASE("Debug print service traps reach the kernel hook with the text address and length",
              "[ppc][kernel_hooks]") {
  HooksGuard guard;
  rex::ppc::SetKernelHooks(nullptr);
  g_debug_text_writes = 0;
  const rex::ppc::KernelHooks fake = {
      nullptr, nullptr, nullptr, nullptr, nullptr, FakeWriteDebugText,
  };
  PPCContext ctx{};
  ctx.r3.u64 = 0x82000100;
  ctx.r4.u64 = 0xABCD0012;

  SECTION("Without hooks the trap writes nothing") {
    rex::ppc::Trap(ctx, nullptr, 20);
    rex::ppc::Trap(ctx, nullptr, 26);
    CHECK(g_debug_text_writes == 0);
  }

  SECTION("With hooks traps 20 and 26 pass r3 and the low half word of r4") {
    rex::ppc::SetKernelHooks(&fake);
    rex::ppc::Trap(ctx, nullptr, 20);
    CHECK(g_debug_text_writes == 1);
    CHECK(g_debug_text_address == 0x82000100);
    CHECK(g_debug_text_length == 0x12);
    rex::ppc::Trap(ctx, nullptr, 26);
    CHECK(g_debug_text_writes == 2);
    rex::ppc::Trap(ctx, nullptr, 25);
    CHECK(g_debug_text_writes == 2);
  }
}
