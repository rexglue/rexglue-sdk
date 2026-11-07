/**
 * @file        tests/unit/kernel/fp_state_test.cpp
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

#include <rex/ppc/context.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/object/kernel_thread.h>

#include "kernel_fixture.h"
#include "kernel/title_state.h"

using rex::u32;
using rex::u64;
using rex::u8;
using rex::ppc::FPSCRPlatform;
using rex::test::Caller;
using rex::test::KernelFixture;

namespace {

void ScrambleFPDPC(PPCContext& ctx, u8*) {
  ctx.fpscr.StoreToHost(rex::ppc::kRoundTowardZero);
  ctx.fpscr.EnableFlushModeUnconditional();
  ctx.fpscr.SetCSR(ctx.fpscr.csr ^ FPSCRPlatform::kExceptionMask);
  ctx.f1.f64 = 99.0;
  ctx.r3.u64 = 0;
}

}  // namespace

REX_TEST_CASE("A DPC that changes the FP control word leaves the draining thread's word intact",
              "[kernel][fp]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 thunk =
        StateOf(c.GetRuntime()).GetFunctionDispatcher()->AllocateThunk(&ScrambleFPDPC, 0);
    CHECKED_ELSE(thunk != 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 dpc = c.Alloc(0x20);
    c.Kernel("KeInitializeDpc", {dpc, thunk, 0});
    c.GetContext().f1.f64 = 1.0;
    const u32 before = FPSCRPlatform::GetCSR();
    CHECK(c.Kernel("KeInsertQueueDpc", {dpc, 0, 0}) == 1);
    CHECK(FPSCRPlatform::GetCSR() == before);
    CHECK(c.GetContext().fpscr.csr == before);
    CHECK(c.GetContext().f1.f64 == 1.0);
  });
}

REX_TEST_CASE(
    "KeSaveFloatingPointState and KeRestoreFloatingPointState round trip f0 to f31 and FPSCR",
    "[kernel][fp]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    PPCContext& ctx = c.GetContext();
    PPCRegister* fpr = &ctx.f0;
    for (u32 i = 0; i < 32; ++i) {
      fpr[i].f64 = 0.5 + i;
    }
    ctx.fpscr.StoreToHost(rex::ppc::kRoundUp);
    const u32 csr = FPSCRPlatform::GetCSR();
    c.Kernel("KeSaveFloatingPointState");
    for (u32 i = 0; i < 32; ++i) {
      fpr[i].f64 = -1.0;
    }
    ctx.fpscr.StoreToHost(rex::ppc::kRoundDown);
    c.Kernel("KeRestoreFloatingPointState");
    for (u32 i = 0; i < 32; ++i) {
      CHECK(fpr[i].f64 == 0.5 + i);
    }
    CHECK(FPSCRPlatform::GetCSR() == csr);
    CHECK(ctx.fpscr.csr == csr);
    ctx.fpscr.StoreToHost(rex::ppc::kRoundNearest);
  });
}

REX_TEST_CASE("Every guest rounding mode reads back from the host control word", "[kernel][fp]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    PPCContext& ctx = c.GetContext();
    for (const u32 mode : {rex::ppc::kRoundNearest, rex::ppc::kRoundTowardZero, rex::ppc::kRoundUp,
                           rex::ppc::kRoundDown}) {
      INFO("rounding mode " << mode);
      ctx.fpscr.StoreToHost(mode);
      CHECK(ctx.fpscr.LoadFromHost() == mode);
    }
    ctx.fpscr.StoreToHost(rex::ppc::kRoundNearest);
  });
}

REX_TEST_CASE("KeSaveVectorUnitState and KeRestoreVectorUnitState round trip v0 to v127",
              "[kernel][fp]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    PPCContext& ctx = c.GetContext();
    PPCVRegister* vr = &ctx.v0;
    for (u32 i = 0; i < 128; ++i) {
      std::memset(&vr[i], static_cast<int>(i), sizeof(PPCVRegister));
    }
    ctx.vscr.sat = 1;
    ctx.vscr.nj = 1;
    c.Kernel("KeSaveVectorUnitState");
    for (u32 i = 0; i < 128; ++i) {
      std::memset(&vr[i], 0xEE, sizeof(PPCVRegister));
    }
    ctx.vscr.sat = 0;
    ctx.vscr.nj = 0;
    c.Kernel("KeRestoreVectorUnitState");
    for (u32 i = 0; i < 128; ++i) {
      const u64 expected = 0x0101010101010101ull * i;
      CHECK(vr[i].u64[0] == expected);
      CHECK(vr[i].u64[1] == expected);
    }
    CHECK(ctx.vscr.sat == 1);
    CHECK(ctx.vscr.nj == 1);
  });
}

REX_TEST_CASE("A host worker starts with every FP exception masked", "[kernel][fp]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto csr = std::make_shared<std::atomic<u32>>(0);
    auto host = std::make_shared<std::atomic<u32>>(0);
    auto thread =
        std::make_unique<rex::kernel::KernelThread>(c.GetRuntime(), 0x20000, 0, [csr, host]() {
          csr->store(rex::kernel::ThreadState::Get()->GetContext()->fpscr.csr);
          host->store(FPSCRPlatform::GetCSR());
          return 0;
        });
    auto launched = c.GetRuntime().GetThreadRegistry().Launch(std::move(thread));
    CHECKED_ELSE(launched) {
      FAIL_CHECK("the check above failed");
      return;
    }
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {*launched, 1, 0, c.AllocQuad(-50000000)}) == 0);
    c.Kernel("NtClose", {*launched});
    CHECK(csr->load() == host->load());
    CHECK((csr->load() & FPSCRPlatform::kExceptionMask) == FPSCRPlatform::kExceptionsMasked);
  });
}

REX_TEST_CASE("A guest thread starts with every FP exception masked", "[kernel][fp]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto csr = std::make_shared<std::atomic<u32>>(0);
    auto host = std::make_shared<std::atomic<u32>>(0);
    auto done = std::make_shared<std::atomic<bool>>(false);
    const u32 thread = c.Spawn([csr, host, done](Caller& t) {
      csr->store(t.GetContext().fpscr.csr);
      host->store(FPSCRPlatform::GetCSR());
      done->store(true);
    });
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {thread, 1, 0, c.AllocQuad(-50000000)}) == 0);
    c.Kernel("NtClose", {thread});
    CHECK(done->load());
    CHECK(csr->load() == host->load());
    CHECK((csr->load() & FPSCRPlatform::kExceptionMask) == FPSCRPlatform::kExceptionsMasked);
  });
}

REX_TEST_CASE("InitHostExceptions masks every FP exception", "[kernel][fp]") {
  constexpr u32 kRound = static_cast<u32>(FPSCRPlatform::kRoundMaskVal);
  u32 word = (FPSCRPlatform::kExceptionsMasked ^ FPSCRPlatform::kExceptionMask) | kRound;
  FPSCRPlatform::InitHostExceptions(word);
  CHECK((word & FPSCRPlatform::kExceptionMask) == FPSCRPlatform::kExceptionsMasked);
  CHECK((word & kRound) == kRound);
}

REX_TEST_CASE("A thread's FP save area starts with every FP exception masked", "[kernel][fp]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller&) {
    const u32 csr = rex::kernel::ThreadState::Get()->GetDPCSaveArea().csr;
    CHECK((csr & FPSCRPlatform::kExceptionMask) == FPSCRPlatform::kExceptionsMasked);
  });
}

REX_TEST_CASE("A new thread starts with VSCR NJ set", "[kernel][fp]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    CHECK(c.GetContext().vscr.nj == 1);
    CHECK(c.GetContext().vscr.sat == 0);
    auto nj = std::make_shared<std::atomic<u32>>(0);
    auto thread = std::make_unique<rex::kernel::KernelThread>(c.GetRuntime(), 0x20000, 0, [nj]() {
      nj->store(rex::kernel::ThreadState::Get()->GetContext()->vscr.nj);
      return 0;
    });
    auto launched = c.GetRuntime().GetThreadRegistry().Launch(std::move(thread));
    CHECKED_ELSE(launched) {
      FAIL_CHECK("the check above failed");
      return;
    }
    CHECK(c.Kernel("NtWaitForSingleObjectEx", {*launched, 1, 0, c.AllocQuad(-50000000)}) == 0);
    c.Kernel("NtClose", {*launched});
    CHECK(nj->load() == 1);
  });
}
