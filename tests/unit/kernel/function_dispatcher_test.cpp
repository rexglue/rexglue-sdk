/**
 * @file        tests/unit/kernel/function_dispatcher_test.cpp
 * @brief       Thunk allocation, unregister cleanup and trap frame dispatch
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <type_traits>
#include <utility>

#include "rex_test.h"

#include <rex/logging/log_category.h>
#include <rex/math.h>
#include <rex/ppc/context.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/heap.h>

#include "test_memory.h"

namespace {

using rex::test::GetTestMemory;

void DummyFn(PPCContext&, u8*) {}

}  // namespace

REX_TEST_CASE("caller_address routes thunk to caller's module pool", "[kernel][dispatcher]") {
  auto& memory = GetTestMemory();
  rex::kernel::FunctionDispatcher dispatcher(&memory);

  constexpr u32 kModA = 0x82000000u;
  constexpr u32 kCodeSize = 0x10000u;
  constexpr u32 kImageSize = 0x100000u;

  REQUIRE(dispatcher.InitializeFunctionTable(kModA, kCodeSize, kModA, kImageSize));

  constexpr u32 kModB = 0x83000000u;
  REQUIRE(dispatcher.InitializeFunctionTable(kModB, kCodeSize, kModB, kImageSize));

  u32 thunk_a = dispatcher.AllocateThunk(&DummyFn, kModA + 0x100);
  u32 thunk_b = dispatcher.AllocateThunk(&DummyFn, kModB + 0x100);

  CHECK(thunk_a >= kModA + kCodeSize);
  CHECK(thunk_a < kModA + kCodeSize + rex::kernel::FunctionDispatcher::kThunkReserveSize);
  CHECK(thunk_b >= kModB + kCodeSize);
  CHECK(thunk_b < kModB + kCodeSize + rex::kernel::FunctionDispatcher::kThunkReserveSize);

  CHECK(dispatcher.GetFunction(thunk_a) == &DummyFn);
  CHECK(dispatcher.GetFunction(thunk_b) == &DummyFn);
}

REX_TEST_CASE("AllocateThunk(0) uses the entrypoint pool only when explicit",
              "[kernel][dispatcher]") {
  auto& memory = GetTestMemory();
  rex::kernel::FunctionDispatcher dispatcher(&memory);

  constexpr u32 kModA = 0x84000000u;
  constexpr u32 kCodeSize = 0x10000u;
  constexpr u32 kImageSize = 0x100000u;

  REQUIRE(dispatcher.InitializeFunctionTable(kModA, kCodeSize, kModA, kImageSize,
                                             /*is_entrypoint=*/true));

  u32 thunk = dispatcher.AllocateThunk(&DummyFn, 0);
  CHECK(thunk >= kModA + kCodeSize);
  CHECK(thunk < kModA + kCodeSize + rex::kernel::FunctionDispatcher::kThunkReserveSize);
}

REX_TEST_CASE("AllocateThunk(0) rejects when no entrypoint registered", "[kernel][dispatcher]") {
  auto& memory = GetTestMemory();
  rex::kernel::FunctionDispatcher dispatcher(&memory);

  constexpr u32 kModA = 0x88000000u;
  constexpr u32 kCodeSize = 0x10000u;
  constexpr u32 kImageSize = 0x100000u;

  REQUIRE(dispatcher.InitializeFunctionTable(kModA, kCodeSize, kModA, kImageSize));

  CHECK(dispatcher.AllocateThunk(&DummyFn, 0) == 0);
}

REX_TEST_CASE("AllocateThunk rejects caller_address outside any module", "[kernel][dispatcher]") {
  auto& memory = GetTestMemory();
  rex::kernel::FunctionDispatcher dispatcher(&memory);

  constexpr u32 kModA = 0x86000000u;
  constexpr u32 kCodeSize = 0x10000u;
  constexpr u32 kImageSize = 0x100000u;

  REQUIRE(dispatcher.InitializeFunctionTable(kModA, kCodeSize, kModA, kImageSize));

  u32 thunk = dispatcher.AllocateThunk(&DummyFn, 0xDEADBEEFu);
  CHECK(thunk == 0);
}

REX_TEST_CASE("SetFunction rejects image data below code_base", "[kernel][dispatcher]") {
  auto& memory = GetTestMemory();
  rex::kernel::FunctionDispatcher dispatcher(&memory);

  constexpr u32 kImageBase = 0x87000000u;
  constexpr u32 kCodeBase = kImageBase + 0x10000u;
  constexpr u32 kCodeSize = 0x10000u;
  constexpr u32 kImageSize = 0x100000u;

  REQUIRE(dispatcher.InitializeFunctionTable(kCodeBase, kCodeSize, kImageBase, kImageSize));
  CHECK_FALSE(dispatcher.SetFunction(kImageBase + 0x10, &DummyFn));
  CHECK(dispatcher.GetFunction(kImageBase + 0x10) == nullptr);
}

namespace {
constexpr u32 kRegisterModBase = 0x85000000u;
void RegisterOne(rex::kernel::IFunctionRegistrar* registrar) {
  registrar->SetFunction(kRegisterModBase + 0x10, &DummyFn);
}
}  // namespace

REX_TEST_CASE("UnregisterModule clears pool and slots", "[kernel][dispatcher]") {
  auto& memory = GetTestMemory();
  rex::kernel::FunctionDispatcher dispatcher(&memory);

  constexpr u32 kCodeSize = 0x10000u;
  constexpr u32 kImageSize = 0x100000u;

  REQUIRE(dispatcher.InitializeFunctionTable(kRegisterModBase, kCodeSize, kRegisterModBase,
                                             kImageSize));

  dispatcher.RegisterModule("modA", kRegisterModBase, &RegisterOne);

  u32 thunk = dispatcher.AllocateThunk(&DummyFn, kRegisterModBase + 0x100);
  REQUIRE(thunk != 0);
  REQUIRE(dispatcher.GetFunction(kRegisterModBase + 0x10) == &DummyFn);
  REQUIRE(dispatcher.GetFunction(thunk) == &DummyFn);

  auto cleared = dispatcher.UnregisterModule("modA");
  CHECK(cleared == std::pair<u32, u32>{kRegisterModBase + kCodeSize, thunk + 4});
  CHECK(dispatcher.GetFunction(kRegisterModBase + 0x10) == nullptr);
  CHECK(dispatcher.GetFunction(thunk) == nullptr);

  REQUIRE(dispatcher.InitializeFunctionTable(kRegisterModBase, kCodeSize, kRegisterModBase,
                                             kImageSize));
  u32 thunk_after = dispatcher.AllocateThunk(&DummyFn, kRegisterModBase + 0x100);
  CHECK(thunk_after == thunk);
}

REX_TEST_CASE("UnregisterModule on unknown id returns nullopt", "[kernel][dispatcher]") {
  auto& memory = GetTestMemory();
  rex::kernel::FunctionDispatcher dispatcher(&memory);
  CHECK_FALSE(dispatcher.UnregisterModule("nope").has_value());
}

namespace {

void ScribbleFn(PPCContext& ctx, u8*) {
  ctx.r3.u64 = 0x1111;
  ctx.r14.u64 = 0xDEAD14;
  ctx.r31.u64 = 0;
  ctx.cr2.SetRaw(0xF);
  ctx.ctr.u64 = 0xC7;
  ctx.xer.so = 1;
  ctx.msr = 0;
  ctx.f14.f64 = 1.0;
  ctx.v14.u32[0] = 0xAAAA;
}

}  // namespace

REX_TEST_CASE("ExecuteTrap restores the interrupted register state", "[kernel][dispatcher]") {
  auto& memory = GetTestMemory();
  rex::kernel::FunctionDispatcher dispatcher(&memory);

  constexpr u32 kTrapModBase = 0x89000000u;
  constexpr u32 kCodeSize = 0x10000u;
  constexpr u32 kImageSize = 0x100000u;
  REQUIRE(dispatcher.InitializeFunctionTable(kTrapModBase, kCodeSize, kTrapModBase, kImageSize));

  constexpr u32 kAddr = kTrapModBase + 0x40;
  REQUIRE(dispatcher.SetFunction(kAddr, &ScribbleFn));

  constexpr u32 kStackSize = 0x10000u;
  const u32 stack = memory.SystemHeapAlloc(kStackSize);
  REQUIRE(stack != 0);
  const u32 pcr = memory.SystemHeapAlloc(0x100);
  *memory.TranslateVirtual<rex::be_u32*>(pcr + 0x74) = stack;
  rex::kernel::ThreadState thread_state(1, stack + kStackSize, pcr, &memory, nullptr);
  auto* ctx = thread_state.GetContext();
  ctx->r14.u64 = 0x14141414;
  ctx->r31.u64 = 0x2E0B57D0;
  ctx->cr2.SetRaw(0x2);
  ctx->ctr.u64 = 0xC0FFEE;
  ctx->xer.so = 0;
  ctx->f14.f64 = 2.5;
  ctx->v14.u32[0] = 0x5555;
  const u32 saved_msr = ctx->msr;
  const u64 saved_r1 = ctx->r1.u64;

  u64 args[] = {0};
  dispatcher.ExecuteTrap(&thread_state, kAddr, args, rex::CountOf(args));

  CHECK(ctx->r14.u64 == 0x14141414);
  CHECK(ctx->r31.u64 == 0x2E0B57D0);
  CHECK(ctx->cr2.GetRaw() == 0x2);
  CHECK(ctx->ctr.u64 == 0xC0FFEE);
  CHECK(ctx->xer.so == 0);
  CHECK(ctx->msr == saved_msr);
  CHECK(ctx->f14.f64 == 2.5);
  CHECK(ctx->v14.u32[0] == 0x5555);
  CHECK(ctx->r1.u64 == saved_r1);
  memory.SystemHeapFree(pcr);
  memory.SystemHeapFree(stack);
}

REX_TEST_CASE("nonvolatile save area round trips cr2-cr5 and fpscr", "[kernel][context]") {
  PPCContext ctx{};
  ctx.r31.u64 = 0x31313131;
  ctx.cr2.SetRaw(0x5);
  ctx.cr3.SetRaw(0xA);
  ctx.cr4.SetRaw(0x3);
  ctx.cr5.SetRaw(0x6);
  ctx.f14.f64 = 4.25;
  ctx.v64.u32[0] = 0x64646464;
  ctx.fpscr.csr = ctx.fpscr.GetCSR();
  const u32 saved_csr = ctx.fpscr.csr;

  u8 area[PPCContext::kNonVolatileSaveSize];
  ctx.SaveNonVolatiles(area);

  ctx.r31.u64 = 0;
  ctx.cr2.SetRaw(0);
  ctx.cr3.SetRaw(0);
  ctx.cr4.SetRaw(0);
  ctx.cr5.SetRaw(0);
  ctx.f14.f64 = 0.0;
  ctx.v64.u32[0] = 0;
  ctx.fpscr.csr = 0;

  ctx.RestoreNonVolatiles(area);

  CHECK(ctx.r31.u64 == 0x31313131);
  CHECK(ctx.cr2.GetRaw() == 0x5);
  CHECK(ctx.cr3.GetRaw() == 0xA);
  CHECK(ctx.cr4.GetRaw() == 0x3);
  CHECK(ctx.cr5.GetRaw() == 0x6);
  CHECK(ctx.f14.f64 == 4.25);
  CHECK(ctx.v64.u32[0] == 0x64646464);
  CHECK(ctx.fpscr.csr == saved_csr);
  CHECK(ctx.fpscr.GetCSR() == saved_csr);
}

REX_TEST_CASE("restoring a zeroed fpscr leaves host FP exception masks intact",
              "[kernel][context]") {
  PPCContext ctx{};
  const u32 host_before = ctx.fpscr.GetCSR();

  u8 area[PPCContext::kNonVolatileSaveSize] = {};
  ctx.RestoreNonVolatiles(area);

  constexpr u32 kGuestMask = PPCFPSCRRegister::kGuestMask;
  CHECK((ctx.fpscr.GetCSR() & ~kGuestMask) == (host_before & ~kGuestMask));
  CHECK(ctx.fpscr.csr == ctx.fpscr.GetCSR());
}

REX_TEST_CASE("FunctionDispatcher deletes through a base pointer", "[kernel]") {
  STATIC_REQUIRE(std::has_virtual_destructor_v<rex::kernel::FunctionDispatcher>);
}
