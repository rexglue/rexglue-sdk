/**
 * @file        tests/unit/system/function_dispatcher_test.cpp
 * @brief       Unit tests for thunk allocation, unregister cleanup, and trap-frame dispatch
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ppc/context.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

#include "test_memory.h"

namespace {

using rex::testing::GetTestMemory;

void DummyFn(PPCContext&, uint8_t*) {}

}  // namespace

TEST_CASE("FunctionDispatcher: caller_address routes thunk to caller's module pool",
          "[runtime][dispatcher]") {
  auto& memory = GetTestMemory();
  rex::runtime::FunctionDispatcher dispatcher(&memory);

  constexpr uint32_t kModA = 0x82000000u;
  constexpr uint32_t kCodeSize = 0x10000u;
  constexpr uint32_t kImageSize = 0x100000u;

  REQUIRE(dispatcher.InitializeFunctionTable(kModA, kCodeSize, kModA, kImageSize));

  constexpr uint32_t kModB = 0x83000000u;
  REQUIRE(dispatcher.InitializeFunctionTable(kModB, kCodeSize, kModB, kImageSize));

  uint32_t thunk_a = dispatcher.AllocateThunk(&DummyFn, kModA + 0x100);
  uint32_t thunk_b = dispatcher.AllocateThunk(&DummyFn, kModB + 0x100);

  CHECK(thunk_a >= kModA + kCodeSize);
  CHECK(thunk_a < kModA + kCodeSize + rex::runtime::FunctionDispatcher::kThunkReserveSize);
  CHECK(thunk_b >= kModB + kCodeSize);
  CHECK(thunk_b < kModB + kCodeSize + rex::runtime::FunctionDispatcher::kThunkReserveSize);

  CHECK(dispatcher.GetFunction(thunk_a) == &DummyFn);
  CHECK(dispatcher.GetFunction(thunk_b) == &DummyFn);
}

TEST_CASE("FunctionDispatcher: AllocateThunk(0) uses the entrypoint pool only when explicit",
          "[runtime][dispatcher]") {
  // caller_address=0 is reserved for host-initiated allocations that have no
  // guest caller (the entrypoint wiring its own __imp__* exports during
  // setup). It must land in the entrypoint module's pool.
  auto& memory = GetTestMemory();
  rex::runtime::FunctionDispatcher dispatcher(&memory);

  constexpr uint32_t kModA = 0x84000000u;
  constexpr uint32_t kCodeSize = 0x10000u;
  constexpr uint32_t kImageSize = 0x100000u;

  REQUIRE(dispatcher.InitializeFunctionTable(kModA, kCodeSize, kModA, kImageSize,
                                             /*is_entrypoint=*/true));

  uint32_t thunk = dispatcher.AllocateThunk(&DummyFn, 0);
  CHECK(thunk >= kModA + kCodeSize);
  CHECK(thunk < kModA + kCodeSize + rex::runtime::FunctionDispatcher::kThunkReserveSize);
}

TEST_CASE("FunctionDispatcher: AllocateThunk(0) rejects when no entrypoint registered",
          "[runtime][dispatcher]") {
  auto& memory = GetTestMemory();
  rex::runtime::FunctionDispatcher dispatcher(&memory);

  constexpr uint32_t kModA = 0x88000000u;
  constexpr uint32_t kCodeSize = 0x10000u;
  constexpr uint32_t kImageSize = 0x100000u;

  REQUIRE(dispatcher.InitializeFunctionTable(kModA, kCodeSize, kModA, kImageSize));

  CHECK(dispatcher.AllocateThunk(&DummyFn, 0) == 0);
}

TEST_CASE("FunctionDispatcher: AllocateThunk rejects caller_address outside any module",
          "[runtime][dispatcher]") {
  auto& memory = GetTestMemory();
  rex::runtime::FunctionDispatcher dispatcher(&memory);

  constexpr uint32_t kModA = 0x86000000u;
  constexpr uint32_t kCodeSize = 0x10000u;
  constexpr uint32_t kImageSize = 0x100000u;

  REQUIRE(dispatcher.InitializeFunctionTable(kModA, kCodeSize, kModA, kImageSize));

  // A non-zero caller_address that doesn't fall inside any module is a bug
  // in the caller: the right answer is to refuse, not to silently route the
  // thunk to the entrypoint pool.
  uint32_t thunk = dispatcher.AllocateThunk(&DummyFn, 0xDEADBEEFu);
  CHECK(thunk == 0);
}

TEST_CASE("FunctionDispatcher: SetFunction rejects image data below code_base",
          "[runtime][dispatcher]") {
  auto& memory = GetTestMemory();
  rex::runtime::FunctionDispatcher dispatcher(&memory);

  constexpr uint32_t kImageBase = 0x87000000u;
  constexpr uint32_t kCodeBase = kImageBase + 0x10000u;
  constexpr uint32_t kCodeSize = 0x10000u;
  constexpr uint32_t kImageSize = 0x100000u;

  REQUIRE(dispatcher.InitializeFunctionTable(kCodeBase, kCodeSize, kImageBase, kImageSize));
  CHECK_FALSE(dispatcher.SetFunction(kImageBase + 0x10, &DummyFn));
  CHECK(dispatcher.GetFunction(kImageBase + 0x10) == nullptr);
}

namespace {
constexpr uint32_t kRegisterModBase = 0x85000000u;
void RegisterOne(rex::runtime::IModuleRegistrar* registrar) {
  registrar->SetFunction(kRegisterModBase + 0x10, &DummyFn);
}
}  // namespace

TEST_CASE("FunctionDispatcher: UnregisterModule clears pool and slots", "[runtime][dispatcher]") {
  auto& memory = GetTestMemory();
  rex::runtime::FunctionDispatcher dispatcher(&memory);

  constexpr uint32_t kCodeSize = 0x10000u;
  constexpr uint32_t kImageSize = 0x100000u;

  REQUIRE(dispatcher.InitializeFunctionTable(kRegisterModBase, kCodeSize, kRegisterModBase,
                                             kImageSize));

  dispatcher.RegisterModule("modA", kRegisterModBase, &RegisterOne);

  uint32_t thunk = dispatcher.AllocateThunk(&DummyFn, kRegisterModBase + 0x100);
  REQUIRE(thunk != 0);
  REQUIRE(dispatcher.GetFunction(kRegisterModBase + 0x10) == &DummyFn);
  REQUIRE(dispatcher.GetFunction(thunk) == &DummyFn);

  auto cleared = dispatcher.UnregisterModule("modA");
  REQUIRE(cleared.has_value());
  CHECK(cleared->first == kRegisterModBase + kCodeSize);
  CHECK(cleared->second == thunk + 4);
  CHECK(dispatcher.GetFunction(kRegisterModBase + 0x10) == nullptr);
  CHECK(dispatcher.GetFunction(thunk) == nullptr);

  // Re-init must succeed: UnregisterModule destroys the per-module table so the
  // same code range can be reloaded without tripping the overlap check.
  REQUIRE(dispatcher.InitializeFunctionTable(kRegisterModBase, kCodeSize, kRegisterModBase,
                                             kImageSize));
  uint32_t thunk_after = dispatcher.AllocateThunk(&DummyFn, kRegisterModBase + 0x100);
  CHECK(thunk_after == thunk);
}

TEST_CASE("FunctionDispatcher: UnregisterModule on unknown id returns nullopt",
          "[runtime][dispatcher]") {
  auto& memory = GetTestMemory();
  rex::runtime::FunctionDispatcher dispatcher(&memory);
  CHECK_FALSE(dispatcher.UnregisterModule("nope").has_value());
}

namespace {

void ScribbleFn(PPCContext& ctx, uint8_t*) {
  ctx.r3.u64 = 0x1111;
  ctx.r14.u64 = 0xDEAD14;
  ctx.r31.u64 = 0;
  ctx.cr2.set_raw(0xF);
  ctx.ctr.u64 = 0xC7;
  ctx.xer.so = 1;
  ctx.msr = 0;
  ctx.f14.f64 = 1.0;
  ctx.v14.u32[0] = 0xAAAA;
}

}  // namespace

TEST_CASE("FunctionDispatcher: ExecuteTrap restores the interrupted register state",
          "[runtime][dispatcher]") {
  auto& memory = GetTestMemory();
  rex::runtime::FunctionDispatcher dispatcher(&memory);

  constexpr uint32_t kTrapModBase = 0x89000000u;
  constexpr uint32_t kCodeSize = 0x10000u;
  constexpr uint32_t kImageSize = 0x100000u;
  REQUIRE(dispatcher.InitializeFunctionTable(kTrapModBase, kCodeSize, kTrapModBase, kImageSize));

  constexpr uint32_t kAddr = kTrapModBase + 0x40;
  REQUIRE(dispatcher.SetFunction(kAddr, &ScribbleFn));

  rex::runtime::ThreadState thread_state(1, 0x70000000u, 0x60000000u, &memory);
  auto* ctx = thread_state.context();
  ctx->r14.u64 = 0x14141414;
  ctx->r31.u64 = 0x2E0B57D0;
  ctx->cr2.set_raw(0x2);
  ctx->ctr.u64 = 0xC0FFEE;
  ctx->xer.so = 0;
  ctx->f14.f64 = 2.5;
  ctx->v14.u32[0] = 0x5555;
  const uint32_t saved_msr = ctx->msr;
  const uint64_t saved_r1 = ctx->r1.u64;

  uint64_t args[] = {0};
  dispatcher.ExecuteTrap(&thread_state, kAddr, args, rex::countof(args));

  CHECK(ctx->r14.u64 == 0x14141414);
  CHECK(ctx->r31.u64 == 0x2E0B57D0);
  CHECK(ctx->cr2.raw() == 0x2);
  CHECK(ctx->ctr.u64 == 0xC0FFEE);
  CHECK(ctx->xer.so == 0);
  CHECK(ctx->msr == saved_msr);
  CHECK(ctx->f14.f64 == 2.5);
  CHECK(ctx->v14.u32[0] == 0x5555);
  CHECK(ctx->r1.u64 == saved_r1);
}

TEST_CASE("PPCContext: non-volatile save area round-trips cr2-cr4 and fpscr",
          "[runtime][context]") {
  PPCContext ctx{};
  ctx.r31.u64 = 0x31313131;
  ctx.cr2.set_raw(0x5);
  ctx.cr3.set_raw(0xA);
  ctx.cr4.set_raw(0x3);
  ctx.f14.f64 = 4.25;
  ctx.v64.u32[0] = 0x64646464;
  ctx.fpscr.csr = ctx.fpscr.getcsr();
  const uint32_t saved_csr = ctx.fpscr.csr;

  uint8_t area[PPCContext::kNonVolatileSaveSize];
  ctx.SaveNonVolatiles(area);

  ctx.r31.u64 = 0;
  ctx.cr2.set_raw(0);
  ctx.cr3.set_raw(0);
  ctx.cr4.set_raw(0);
  ctx.f14.f64 = 0.0;
  ctx.v64.u32[0] = 0;
  ctx.fpscr.csr = 0;

  ctx.RestoreNonVolatiles(area);

  CHECK(ctx.r31.u64 == 0x31313131);
  CHECK(ctx.cr2.raw() == 0x5);
  CHECK(ctx.cr3.raw() == 0xA);
  CHECK(ctx.cr4.raw() == 0x3);
  CHECK(ctx.f14.f64 == 4.25);
  CHECK(ctx.v64.u32[0] == 0x64646464);
  CHECK(ctx.fpscr.csr == saved_csr);
  CHECK(ctx.fpscr.getcsr() == saved_csr);
}

TEST_CASE("PPCContext: restoring a zeroed fpscr leaves host FP exception masks intact",
          "[runtime][context]") {
  PPCContext ctx{};
  const uint32_t host_before = ctx.fpscr.getcsr();

  uint8_t area[PPCContext::kNonVolatileSaveSize] = {};
  ctx.RestoreNonVolatiles(area);

  constexpr uint32_t kGuestMask = PPCFPSCRRegister::GuestMask;
  CHECK((ctx.fpscr.getcsr() & ~kGuestMask) == (host_before & ~kGuestMask));
  CHECK(ctx.fpscr.csr == ctx.fpscr.getcsr());
}
