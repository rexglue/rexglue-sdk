/**
 * @file        tests/unit/kernel/char_fp_state_test.cpp
 * @brief       FP and vector state across DPCs and the Ke save exports
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
#include <memory>

#include <rex/platform/fpscr.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/thread_registry.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xthread.h>

#include "kernel_fixture.h"

using rex::u32;
using rex::u64;
using rex::u8;
using rex::platform::FPSCRPlatform;
using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {

void ScrambleFPDPC(PPCContext& ctx, u8*) {
  ctx.fpscr.storeFromGuest(rex::ppc::kRoundTowardZero);
  ctx.fpscr.enableFlushModeUnconditional();
  ctx.fpscr.setcsr(ctx.fpscr.csr ^ FPSCRPlatform::ExceptionMask);
  ctx.f1.f64 = 99.0;
  ctx.r3.u64 = 0;
}

}  // namespace

REX_TEST_CASE("A DPC that changes the FP control word leaves the draining thread's word intact",
              "[kernel][fp]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 thunk = c.runtime().function_dispatcher()->AllocateThunk(&ScrambleFPDPC, 0);
    CHECKED_ELSE(thunk != 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 dpc = c.Alloc(0x20);
    c.Kernel("KeInitializeDpc", {dpc, thunk, 0});
    c.ctx().f1.f64 = 1.0;
    const u32 before = FPSCRPlatform::getcsr();
    CHECK(c.Kernel("KeInsertQueueDpc", {dpc, 0, 0}) == 1);
    CHECK(FPSCRPlatform::getcsr() == before);
    CHECK(c.ctx().fpscr.csr == before);
    CHECK(c.ctx().f1.f64 == 1.0);
  });
}

REX_TEST_CASE(
    "KeSaveFloatingPointState and KeRestoreFloatingPointState round trip f0 to f31 and FPSCR",
    "[kernel][fp]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    PPCContext& ctx = c.ctx();
    PPCRegister* fpr = &ctx.f0;
    for (u32 i = 0; i < 32; ++i) {
      fpr[i].f64 = 0.5 + i;
    }
    ctx.fpscr.storeFromGuest(rex::ppc::kRoundUp);
    const u32 csr = FPSCRPlatform::getcsr();
    c.Kernel("KeSaveFloatingPointState");
    for (u32 i = 0; i < 32; ++i) {
      fpr[i].f64 = -1.0;
    }
    ctx.fpscr.storeFromGuest(rex::ppc::kRoundDown);
    c.Kernel("KeRestoreFloatingPointState");
    for (u32 i = 0; i < 32; ++i) {
      CHECK(fpr[i].f64 == 0.5 + i);
    }
    CHECK(FPSCRPlatform::getcsr() == csr);
    CHECK(ctx.fpscr.csr == csr);
    ctx.fpscr.storeFromGuest(rex::ppc::kRoundNearest);
  });
}

REX_TEST_CASE("Every guest rounding mode reads back from the host control word", "[kernel][fp]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    PPCContext& ctx = c.ctx();
    for (const u32 mode : {rex::ppc::kRoundNearest, rex::ppc::kRoundTowardZero, rex::ppc::kRoundUp,
                           rex::ppc::kRoundDown}) {
      INFO("rounding mode " << mode);
      ctx.fpscr.storeFromGuest(mode);
      CHECK(ctx.fpscr.loadFromHost() == mode);
    }
    ctx.fpscr.storeFromGuest(rex::ppc::kRoundNearest);
  });
}

REX_TEST_CASE("KeSaveVectorUnitState and KeRestoreVectorUnitState round trip v0 to v127",
              "[kernel][fp]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    PPCContext& ctx = c.ctx();
    PPCVRegister* vr = &ctx.v0;
    for (u32 i = 0; i < 128; ++i) {
      std::memset(&vr[i], static_cast<int>(i), sizeof(PPCVRegister));
    }
    ctx.vscr_sat = 1;
    c.Kernel("KeSaveVectorUnitState");
    for (u32 i = 0; i < 128; ++i) {
      std::memset(&vr[i], 0xEE, sizeof(PPCVRegister));
    }
    ctx.vscr_sat = 0;
    c.Kernel("KeRestoreVectorUnitState");
    for (u32 i = 0; i < 128; ++i) {
      const u64 expected = 0x0101010101010101ull * i;
      CHECK(vr[i].u64[0] == expected);
      CHECK(vr[i].u64[1] == expected);
    }
    CHECK(ctx.vscr_sat == 1);
  });
}

REX_TEST_CASE("A host worker starts from the host policy FP control word", "[kernel][fp]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto csr = std::make_shared<std::atomic<u32>>(0);
    auto host = std::make_shared<std::atomic<u32>>(0);
    auto thread =
        std::make_unique<rex::runtime::KernelThread>(c.runtime(), 0x20000, 0, [csr, host]() {
          csr->store(rex::runtime::ThreadState::Get()->context()->fpscr.csr);
          host->store(FPSCRPlatform::getcsr());
          return 0;
        });
    auto launched = c.runtime().thread_registry().Launch(std::move(thread));
    CHECKED_ELSE(launched) {
      FAIL_CHECK("the check above failed");
      return;
    }
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {*launched, 1, 0, c.Quad(-50000000)}) == 0);
    c.Kernel("NtClose", {*launched});
    u32 policy = csr->load();
    FPSCRPlatform::InitHostExceptions(policy);
    CHECK(csr->load() == host->load());
    CHECK(policy == csr->load());
  });
}

REX_TEST_CASE("A thread's FP save area starts with every FP exception masked", "[kernel][fp]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller&) {
    u32 expected = 0;
    FPSCRPlatform::InitHostExceptions(expected);
    CHECK(rex::runtime::ThreadState::Get()->dpc_save_area().csr == expected);
  });
}
