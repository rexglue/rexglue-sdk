/**
 * @file        tests/unit/kernel/call_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <span>

#include <rex/ppc/hook.h>
#include <rex/memory/endian.h>
#include <rex/ppc/context.h>
#include <rex/ppc/call.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/heap.h>

#include "../kernel/kernel_fixture.h"
#include "test_memory.h"
#include "kernel/ppc_hooks.h"
#include "kernel/title_state.h"
#include <memory>
#include "kernel_fixture.h"

extern "C" void rexshim_ConvertThreadToFiber(PPCContext& ctx, rex::u8* base);
extern "C" void rexshim_ConvertFiberToThread(PPCContext& ctx, rex::u8* base);
extern "C" void rexshim_CreateFiber(PPCContext& ctx, rex::u8* base);
extern "C" void rexshim_DeleteFiber(PPCContext& ctx, rex::u8* base);
extern "C" void rexshim_SwitchToFiber(PPCContext& ctx, rex::u8* base);

using rex::be_u32;
using rex::f32;
using rex::f64;
using rex::u32;
using rex::u64;
using rex::u8;
using rex::kernel::ThreadState;

namespace {

constexpr u32 kFrameSize = 0x50;
constexpr u32 kSentinel = 0x5E000000;
constexpr u32 kStackSize = 0x10000;
u32 g_pcr = 0;
constexpr u32 kDepth = 3;

enum class Mode : u8 { kImport, kGuestToHost, kExecute };

Mode g_mode = Mode::kImport;
u32 g_failures = 0;
u32 g_deepest = 0;
std::array<u32, kDepth + 1> g_frames{};
rex::kernel::FunctionDispatcher* g_dispatcher = nullptr;
u32 g_guest_address = 0;

void GuestOpen(PPCContext& ctx, u8* base);
void HookNext(u32 depth);

rex::ppc::ImportFunction<void(u32)> g_guest{GuestOpen};

void GuestOpen(PPCContext& ctx, u8* base) {
  const u32 depth = ctx.r3.u32;
  if (ctx.r13.u32 != g_pcr || &ctx != ThreadState::Get()->GetContext()) {
    ++g_failures;
  }
  ctx.r1.u32 -= kFrameSize;
  const u32 frame = ctx.r1.u32;
  g_frames[depth] = frame;
  rex::memory::store<be_u32>(base + frame, kSentinel + depth);
  ctx.r3.u64 = depth;
  rex::ppc::GuestToHostThunk<&HookNext>(ctx, base);
  if (rex::memory::load<be_u32>(base + frame) != kSentinel + depth || ctx.r1.u32 != frame) {
    ++g_failures;
  }
  ctx.r1.u32 += kFrameSize;
}

void HookNext(u32 depth) {
  g_deepest = std::max(g_deepest, depth);
  if (depth >= kDepth) {
    return;
  }
  auto* state = ThreadState::Get();
  switch (depth == 1 ? g_mode : Mode::kImport) {
    case Mode::kImport:
      g_guest(depth + 1);
      break;
    case Mode::kGuestToHost:
      rex::ppc::HostToGuestCall<void>(GuestOpen, depth + 1);
      break;
    case Mode::kExecute: {
      u64 args[] = {depth + 1};
      g_dispatcher->Execute(state, g_guest_address, args, 1);
      break;
    }
  }
}

u32 AllocPCR(rex::kernel::GuestAddressSpace& memory, u32 stack) {
  const u32 pcr = memory.SystemHeapAlloc(0x100);
  *memory.TranslateVirtual<be_u32*>(pcr + 0x70) = stack + kStackSize;
  *memory.TranslateVirtual<be_u32*>(pcr + 0x74) = stack;
  g_pcr = pcr;
  return pcr;
}

struct GuestThread {
  rex::kernel::ScopedPPCHookRegistration hooks;
  rex::kernel::GuestAddressSpace& memory = rex::test::GetTestMemory();
  u32 stack = memory.SystemHeapAlloc(kStackSize);
  u32 pcr = AllocPCR(memory, stack);
  ThreadState state{1, stack + kStackSize, pcr, &memory, nullptr};
  ThreadState* previous = ThreadState::Get();

  GuestThread() {
    ThreadState::Bind(&state);
    g_failures = 0;
    g_deepest = 0;
    g_frames = {};
  }
  ~GuestThread() {
    ThreadState::Bind(previous);
    memory.SystemHeapFree(pcr);
    memory.SystemHeapFree(stack);
  }
};

void CheckNested(GuestThread& thread) {
  CHECK(g_failures == 0);
  CHECK(g_deepest == kDepth);
  for (u32 depth = 1; depth < kDepth; ++depth) {
    CHECK(g_frames[depth + 1] + kFrameSize <= g_frames[depth]);
  }
  CHECK(thread.state.GetContext()->r1.u32 == thread.stack + kStackSize);
}

}  // namespace

REX_TEST_CASE("Nested calls keep every title frame", "[kernel][call]") {
  GuestThread thread;
  g_mode = Mode::kImport;
  g_guest(1);
  CheckNested(thread);
}

REX_TEST_CASE("HostToGuestCall nests under the running frame", "[kernel][call]") {
  GuestThread thread;
  g_mode = Mode::kGuestToHost;
  g_guest(1);
  CheckNested(thread);
}

REX_TEST_CASE("FunctionDispatcher::Execute inside a call runs under the live frame",
              "[kernel][call]") {
  GuestThread thread;
  rex::kernel::FunctionDispatcher dispatcher(&thread.memory);
  constexpr u32 kModule = 0x8A000000;
  REQUIRE(dispatcher.InitializeFunctionTable(kModule, 0x10000, kModule, 0x100000));
  g_guest_address = kModule + 0x40;
  REQUIRE(dispatcher.SetFunction(g_guest_address, &GuestOpen));
  g_dispatcher = &dispatcher;
  g_mode = Mode::kExecute;
  g_guest(1);
  g_dispatcher = nullptr;
  CheckNested(thread);
}

namespace {

constexpr u32 kLeafScratch = 576;
constexpr u32 kRedZoneDeepestOffset = rex::ppc::kRedZone - 4;

void Clobber(PPCContext& ctx, u8* base) {
  ctx.r0.u64 = ctx.r4.u64 = ctx.r11.u64 = ctx.r12.u64 = 0xDEAD;
  ctx.r14.u64 = ctx.r31.u64 = 0xDEAD;
  ctx.f0.f64 = ctx.f13.f64 = ctx.f14.f64 = -1.0;
  ctx.ctr.u64 = 0xDEAD;
  ctx.lr = 0xDEAD;
  ctx.cr0.SetRaw(0xF);
  ctx.cr1.SetRaw(0xF);
  ctx.cr5.SetRaw(0xF);
  ctx.cr7.SetRaw(0xF);
  ctx.xer.so = 0;
  ctx.xer.ca = 0;
  ctx.v0.u64[0] = ctx.v63.u64[0] = 0xDEAD;
  for (u32 offset = 0; offset < kLeafScratch + kFrameSize; offset += 4) {
    rex::memory::store<be_u32>(base + ctx.r1.u32 - kLeafScratch + offset, 0);
  }
  ctx.r3.u64 = 7;
}

rex::ppc::ImportFunction<u32()> g_clobber{Clobber};

}  // namespace

REX_TEST_CASE("A call from a mid function hook keeps the caller's registers and red zone",
              "[kernel][call]") {
  GuestThread thread;
  PPCContext& ctx = *thread.state.GetContext();
  const u32 caller_r1 = ctx.r1.u32;
  u8* base = thread.memory.GetVirtualMembase();
  ctx.r0.u64 = 1;
  ctx.r4.u64 = 4;
  ctx.r11.u64 = 11;
  ctx.r12.u64 = 12;
  ctx.r14.u64 = 14;
  ctx.r31.u64 = 31;
  ctx.f0.f64 = 0.5;
  ctx.f13.f64 = 13.5;
  ctx.f14.f64 = 14.5;
  ctx.ctr.u64 = 0xC7;
  ctx.lr = 0x82001000;
  ctx.cr0.SetRaw(0x2);
  ctx.cr1.SetRaw(0x4);
  ctx.cr5.SetRaw(0x8);
  ctx.cr7.SetRaw(0x1);
  ctx.xer.so = 1;
  ctx.xer.ca = 1;
  ctx.v0.u64[0] = 0x1111;
  ctx.v63.u64[0] = 0x6363;
  const u32 scratch = ctx.r1.u32 - kLeafScratch;
  const u32 deepest = ctx.r1.u32 - kRedZoneDeepestOffset;
  rex::memory::store<be_u32>(base + scratch, kSentinel);
  rex::memory::store<be_u32>(base + deepest, kSentinel + 1);

  ctx.r1.u64 -= rex::ppc::kRedZone;
  CHECK(g_clobber() == 7);
  ctx.r1.u64 += rex::ppc::kRedZone;

  CHECK(ctx.r0.u64 == 1);
  CHECK(ctx.r4.u64 == 4);
  CHECK(ctx.r11.u64 == 11);
  CHECK(ctx.r12.u64 == 12);
  CHECK(ctx.r14.u64 == 14);
  CHECK(ctx.r31.u64 == 31);
  CHECK(ctx.f0.f64 == 0.5);
  CHECK(ctx.f13.f64 == 13.5);
  CHECK(ctx.f14.f64 == 14.5);
  CHECK(ctx.ctr.u64 == 0xC7);
  CHECK(ctx.lr == 0x82001000);
  CHECK(ctx.cr0.GetRaw() == 0x2);
  CHECK(ctx.cr1.GetRaw() == 0x4);
  CHECK(ctx.cr5.GetRaw() == 0x8);
  CHECK(ctx.cr7.GetRaw() == 0x1);
  CHECK(ctx.xer.so == 1);
  CHECK(ctx.xer.ca == 1);
  CHECK(ctx.v0.u64[0] == 0x1111);
  CHECK(ctx.v63.u64[0] == 0x6363);
  CHECK(ctx.r1.u32 == caller_r1);
  CHECK(rex::memory::load<be_u32>(base + scratch) == kSentinel);
  CHECK(rex::memory::load<be_u32>(base + deepest) == kSentinel + 1);
}

namespace {

std::array<u64, 13> g_slots{};
std::array<f64, 3> g_floats{};
f64 g_stack_float = 0;
u32 g_back_chain = 0;
u32 g_callee_r1 = 0;
u64 g_callee_lr = 0;

void ReadArguments(PPCContext& ctx, u8* base) {
  g_slots = {ctx.r3.u64,
             ctx.r4.u64,
             ctx.r5.u64,
             ctx.r6.u64,
             ctx.r7.u64,
             ctx.r8.u64,
             ctx.r9.u64,
             ctx.r10.u64,
             rex::memory::load<be_u32>(base + ctx.r1.u32 + 0x54),
             rex::memory::load<be_u32>(base + ctx.r1.u32 + 0x5C),
             rex::memory::load<be_u32>(base + ctx.r1.u32 + 0x64),
             rex::memory::load<be_u32>(base + ctx.r1.u32 + 0x6C),
             rex::memory::load<be_u32>(base + ctx.r1.u32 + 0x74)};
  g_floats = {ctx.f1.f64, ctx.f2.f64, ctx.f3.f64};
  g_back_chain = rex::memory::load<be_u32>(base + ctx.r1.u32);
  g_callee_r1 = ctx.r1.u32;
  g_callee_lr = ctx.lr;
  ctx.f1.f64 = 2.5;
}

rex::ppc::ImportFunction<f64(u32, f64, u32, u32, f64, u32, u32, u32, u32, u32, f64, u32, u32)>
    g_arguments{ReadArguments};

void ReadFourteenthFloat(PPCContext& ctx, u8* base) {
  g_stack_float = std::bit_cast<f64>(static_cast<u64>(
      rex::memory::load<be_u64>(base + ctx.r1.u32 + 0x50 + std::ptrdiff_t{5} * 8)));
  g_floats = {ctx.f1.f64, ctx.f13.f64, 0};
}

rex::ppc::ImportFunction<void(f64, f64, f64, f64, f64, f64, f64, f64, f64, f64, f64, f64, f64, f64)>
    g_fourteen_floats{ReadFourteenthFloat};

u64 g_export_ints[3]{};
f64 g_export_float = 0;

u32 MixedExport(u32 a, f64 b, u32 c, u32 d) {
  g_export_ints[0] = a;
  g_export_float = b;
  g_export_ints[1] = c;
  g_export_ints[2] = d;
  return a + c + d;
}

void ReturnPointer(PPCContext& ctx, u8*) {
  ctx.r3.u64 = ctx.r3.u64 + 0x10;
}

u32 g_frame_r1 = 0;
u32 g_saved_link_word = 0;
u64 g_seen_r2 = 0;
u64 g_seen_r13 = 0;

void SaveLinkRegister(PPCContext& ctx, u8* base) {
  g_frame_r1 = ctx.r1.u32;
  rex::memory::store<be_u32>(base + ctx.r1.u32 - 8, static_cast<u32>(ctx.lr));
  g_saved_link_word = rex::memory::load<be_u32>(base + ctx.r1.u32 - 8);
}

void CaptureFrame(PPCContext& ctx, u8*) {
  g_frame_r1 = ctx.r1.u32;
}

void CaptureReservedRegisters(PPCContext& ctx, u8*) {
  g_seen_r2 = ctx.r2.u64;
  g_seen_r13 = ctx.r13.u64;
}

rex::ppc::ImportFunction<void()> g_link_saver{SaveLinkRegister};
rex::ppc::ImportFunction<void(u32, u32, u32, u32, u32, u32, u32, u32)> g_eight_slots{CaptureFrame};
rex::ppc::ImportFunction<void(u32, u32, u32, u32, u32, u32, u32, u32, u32)> g_nine_slots{
    CaptureFrame};
rex::ppc::ImportFunction<void()> g_reserved_reader{CaptureReservedRegisters};

}  // namespace

REX_TEST_CASE("A call places integer, float and stack arguments", "[kernel][call]") {
  GuestThread thread;
  g_slots.fill(0xDEAD);
  PPCContext& ctx = *thread.state.GetContext();
  ctx.r4.u64 = 0x44;
  ctx.r7.u64 = 0x77;
  CHECK(g_arguments(1, 0.25, 2, 3, 0.5, 4, 5, 6, 7, 8, 0.75, 9, 10) == 2.5);
  CHECK(g_slots[0] == 1);
  CHECK(g_slots[1] == 0x44);
  CHECK(g_slots[2] == 2);
  CHECK(g_slots[3] == 3);
  CHECK(g_slots[4] == 0x77);
  CHECK(g_slots[5] == 4);
  CHECK(g_slots[6] == 5);
  CHECK(g_slots[7] == 6);
  CHECK(g_slots[8] == 7);
  CHECK(g_slots[9] == 8);
  CHECK(g_slots[11] == 9);
  CHECK(g_slots[12] == 10);
  CHECK(g_floats == std::array<f64, 3>{0.25, 0.5, 0.75});
}

REX_TEST_CASE("The callee saves LR at -8 from its r1 and the call reserves nothing for it",
              "[kernel][call]") {
  GuestThread thread;
  const u32 caller_r1 = thread.state.GetContext()->r1.u32;
  g_link_saver();
  CHECK(g_saved_link_word == rex::ppc::kHostReturnAddress);
  CHECK(caller_r1 - g_frame_r1 == rex::ppc::kLinkageSize);
}

REX_TEST_CASE("A call frame is at least 0x50 bytes and grows past 8 argument slots",
              "[kernel][call]") {
  GuestThread thread;
  const u32 caller_r1 = thread.state.GetContext()->r1.u32;
  g_eight_slots(1, 2, 3, 4, 5, 6, 7, 8);
  CHECK(caller_r1 - g_frame_r1 == 0x50);
  g_nine_slots(1, 2, 3, 4, 5, 6, 7, 8, 9);
  CHECK(caller_r1 - g_frame_r1 == 0x60);
}

REX_TEST_CASE("r2 and r13 are untouched across a host call", "[kernel][call]") {
  GuestThread thread;
  PPCContext& ctx = *thread.state.GetContext();
  const u64 pcr = ctx.r13.u64;
  ctx.r2.u64 = 0x2222AAAA;
  g_reserved_reader();
  CHECK(g_seen_r2 == 0x2222AAAA);
  CHECK(g_seen_r13 == pcr);
  CHECK(ctx.r2.u64 == 0x2222AAAA);
  CHECK(ctx.r13.u64 == pcr);
}

REX_TEST_CASE("A call passes the 14th float in its stack slot", "[kernel][call]") {
  GuestThread thread;
  g_fourteen_floats(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12.0, 13.0, 14.5);
  CHECK(g_floats[0] == 1.0);
  CHECK(g_floats[1] == 13.0);
  CHECK(g_stack_float == 14.5);
}

REX_TEST_CASE("An export reads an integer after a float from the next slot", "[kernel][call]") {
  GuestThread thread;
  PPCContext& ctx = *thread.state.GetContext();
  ctx.r3.u64 = 1;
  ctx.r4.u64 = 0x44;
  ctx.f1.f64 = 0.5;
  ctx.r5.u64 = 2;
  ctx.r6.u64 = 3;
  rex::ppc::GuestToHostThunk<&MixedExport>(ctx,
                                           thread.state.GetAddressSpace()->GetVirtualMembase());
  CHECK(g_export_ints[0] == 1);
  CHECK(g_export_float == 0.5);
  CHECK(g_export_ints[1] == 2);
  CHECK(g_export_ints[2] == 3);
  CHECK(ctx.r3.u64 == 6);
}

REX_TEST_CASE("A call frame is aligned, chained and returns to the host", "[kernel][call]") {
  GuestThread thread;
  const u32 caller_r1 = thread.state.GetContext()->r1.u32;
  g_arguments(1, 0.25, 2, 3, 0.5, 4, 5, 6, 7, 8, 0.75, 9, 10);
  CHECK(g_back_chain == caller_r1);
  CHECK(g_callee_r1 == ((caller_r1 - rex::ppc::kLinkageSize - 5 * 8) & ~0xFu));
  CHECK(g_callee_lr == rex::ppc::kHostReturnAddress);
}

REX_TEST_CASE("Call and HostToGuestCall return guest pointers and integers", "[kernel][call]") {
  GuestThread thread;
  u8* base = thread.memory.GetVirtualMembase();
  rex::ppc::ImportFunction<u8*(u32)> pointer{ReturnPointer};
  CHECK(pointer(thread.stack) == base + thread.stack + 0x10);
  CHECK(pointer(0) == base + 0x10);
  CHECK(rex::ppc::HostToGuestCall<u32>(ReturnPointer, u32{5}) == 0x15);
}

REX_TEST_CASE("FunctionDispatcher::Execute places register and stack arguments", "[kernel][call]") {
  GuestThread thread;
  rex::kernel::FunctionDispatcher dispatcher(&thread.memory);
  constexpr u32 kModule = 0x8B000000;
  REQUIRE(dispatcher.InitializeFunctionTable(kModule, 0x10000, kModule, 0x100000));
  REQUIRE(dispatcher.SetFunction(kModule + 0x40, &ReadArguments));
  u64 args[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  dispatcher.Execute(&thread.state, kModule + 0x40, args, std::size(args));
  CHECK(std::ranges::equal(std::span(g_slots).first(10),
                           std::array<u64, 10>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
  CHECK(g_back_chain == thread.stack + kStackSize);
  CHECK(g_callee_lr == rex::ppc::kHostReturnAddress);
  CHECK(thread.state.GetContext()->r1.u32 == thread.stack + kStackSize);
}

namespace {

constexpr u64 kWideValue = 0x1122334455667788;
u64 g_wide_slot = 0;

void ReadWideArgument(PPCContext& ctx, u8* base) {
  g_wide_slot = rex::memory::load<rex::be_u64>(base + ctx.r1.u32 + 0x50);
}

rex::ppc::ImportFunction<void(u32, u32, u32, u32, u32, u32, u32, u32, u64)> g_wide_argument{
    ReadWideArgument};

}  // namespace

REX_TEST_CASE("A call keeps the high word of a 64 bit stack argument", "[kernel][call]") {
  GuestThread thread;
  g_wide_slot = 0;
  g_wide_argument(1, 2, 3, 4, 5, 6, 7, 8, kWideValue);
  CHECK(g_wide_slot == kWideValue);
}

REX_TEST_CASE("FunctionDispatcher::Execute keeps the high word of a 64 bit stack argument",
              "[kernel][call]") {
  GuestThread thread;
  rex::kernel::FunctionDispatcher dispatcher(&thread.memory);
  constexpr u32 kModule = 0x8C000000;
  REQUIRE(dispatcher.InitializeFunctionTable(kModule, 0x10000, kModule, 0x100000));
  REQUIRE(dispatcher.SetFunction(kModule + 0x40, &ReadWideArgument));
  g_wide_slot = 0;
  u64 args[] = {1, 2, 3, 4, 5, 6, 7, 8, kWideValue};
  dispatcher.Execute(&thread.state, kModule + 0x40, args, std::size(args));
  CHECK(g_wide_slot == kWideValue);
}

namespace {

f64 g_float_register = 0;
f64 g_float_slot = 0;
f32 g_single_slot = 0;
f32 g_single_register = 0;

void ReadDoubleInSlotNine(PPCContext& ctx, u8* base) {
  g_float_register = ctx.f1.f64;
  g_float_slot = std::bit_cast<f64>(
      static_cast<u64>(rex::memory::load<rex::be_u64>(base + ctx.r1.u32 + 0x50)));
}

void ReadSingleInSlotNine(PPCContext& ctx, u8* base) {
  g_single_register = static_cast<f32>(ctx.f1.f64);
  g_single_slot =
      std::bit_cast<f32>(static_cast<u32>(rex::memory::load<be_u32>(base + ctx.r1.u32 + 0x54)));
}

rex::ppc::ImportFunction<void(u32, u32, u32, u32, u32, u32, u32, u32, f64)> g_double_in_slot_nine{
    ReadDoubleInSlotNine};
rex::ppc::ImportFunction<void(u32, u32, u32, u32, u32, u32, u32, u32, f32)> g_single_in_slot_nine{
    ReadSingleInSlotNine};

}  // namespace

REX_TEST_CASE("A double in the 9th slot is written to its register and its stack slot",
              "[kernel][call]") {
  GuestThread thread;
  g_float_register = 0;
  g_float_slot = 0;
  g_double_in_slot_nine(1, 2, 3, 4, 5, 6, 7, 8, 6.25);
  CHECK(g_float_register == 6.25);
  CHECK(g_float_slot == 6.25);
}

REX_TEST_CASE("A 4 byte f32 in the 9th slot sits in the low word of the slot", "[kernel][call]") {
  GuestThread thread;
  g_single_register = 0;
  g_single_slot = 0;
  g_single_in_slot_nine(1, 2, 3, 4, 5, 6, 7, 8, 1.5f);
  CHECK(g_single_register == 1.5f);
  CHECK(g_single_slot == 1.5f);
}

namespace {

PPCVRegister g_vector_in{};
PPCVRegister g_thirteenth_vector{};
PPCVRegister g_stack_vector{};
u64 g_vector_ints[2]{};

PPCVRegister MakeVector(u32 base) {
  PPCVRegister value{};
  for (u32 lane = 0; lane < 4; ++lane) {
    value.u32[lane] = base * 16 + lane;
  }
  return value;
}

bool SameVector(const PPCVRegister& a, const PPCVRegister& b) {
  return std::ranges::equal(a.u32, b.u32);
}

void ReadVectorFirst(PPCContext& ctx, u8*) {
  g_vector_in = ctx.v1;
  g_vector_ints[0] = ctx.r3.u64;
  g_vector_ints[1] = ctx.r4.u64;
}

void ReadFourteenVectors(PPCContext& ctx, u8* base) {
  g_thirteenth_vector = ctx.v13;
  const u8* slot = base + ctx.r1.u32 + 0x50;
  for (u32 i = 0; i < 16; ++i) {
    g_stack_vector.u8[i] = slot[15 - i];
  }
}

void ReturnVector(PPCContext& ctx, u8*) {
  ctx.v1 = MakeVector(ctx.r3.u32);
}

PPCVRegister EchoVector(PPCVRegister value, u32 mask) {
  value.u32[0] ^= mask;
  return value;
}

rex::ppc::ImportFunction<void(PPCVRegister, u32, u32)> g_vector_first{ReadVectorFirst};
rex::ppc::ImportFunction<void(PPCVRegister, PPCVRegister, PPCVRegister, PPCVRegister, PPCVRegister,
                              PPCVRegister, PPCVRegister, PPCVRegister, PPCVRegister, PPCVRegister,
                              PPCVRegister, PPCVRegister, PPCVRegister, PPCVRegister)>
    g_fourteen_vectors{ReadFourteenVectors};
rex::ppc::ImportFunction<PPCVRegister(u32)> g_vector_return{ReturnVector};

}  // namespace

REX_TEST_CASE("A vector argument uses v1 and no integer register", "[kernel][call]") {
  GuestThread thread;
  g_vector_first(MakeVector(3), 7, 9);
  CHECK(SameVector(g_vector_in, MakeVector(3)));
  CHECK(g_vector_ints[0] == 7);
  CHECK(g_vector_ints[1] == 9);
}

REX_TEST_CASE("The 13th vector is in v13 and the 14th is on the stack at 16 bytes",
              "[kernel][call]") {
  GuestThread thread;
  g_fourteen_vectors(MakeVector(1), MakeVector(2), MakeVector(3), MakeVector(4), MakeVector(5),
                     MakeVector(6), MakeVector(7), MakeVector(8), MakeVector(9), MakeVector(10),
                     MakeVector(11), MakeVector(12), MakeVector(13), MakeVector(14));
  CHECK(SameVector(g_thirteenth_vector, MakeVector(13)));
  CHECK(SameVector(g_stack_vector, MakeVector(14)));
}

REX_TEST_CASE("A call returning a vector reads v1", "[kernel][call]") {
  GuestThread thread;
  CHECK(SameVector(g_vector_return(5), MakeVector(5)));
}

REX_TEST_CASE("A hook reads a vector from v1 and returns it in v1", "[kernel][call]") {
  GuestThread thread;
  PPCContext& ctx = *thread.state.GetContext();
  ctx.v1 = MakeVector(2);
  ctx.r3.u64 = 0xFF;
  rex::ppc::GuestToHostThunk<&EchoVector>(ctx, thread.state.GetAddressSpace()->GetVirtualMembase());
  PPCVRegister expected = MakeVector(2);
  expected.u32[0] ^= 0xFF;
  CHECK(SameVector(ctx.v1, expected));
}

namespace {

struct Twelve {
  rex::be_u32 a;
  rex::be_u32 b;
  rex::be_u32 c;
};

struct Four {
  rex::be_u32 a;
};

u64 g_structure_registers[8]{};
u32 g_structure_stack_word = 0;
u32 g_hidden_pointer = 0;
u64 g_hidden_first_argument = 0;

Twelve MakeTwelve(u32 first) {
  return Twelve{first, first + 1, first + 2};
}

void ReadSplitStructure(PPCContext& ctx, u8* base) {
  g_structure_registers[7] = ctx.r10.u64;
  g_structure_stack_word = rex::memory::load<be_u32>(base + ctx.r1.u32 + 0x50);
}

void ReadJustifiedStructures(PPCContext& ctx, u8*) {
  g_structure_registers[0] = ctx.r3.u64;
  g_structure_registers[1] = ctx.r4.u64;
  g_structure_registers[2] = ctx.r5.u64;
}

void ReturnHiddenTwelve(PPCContext& ctx, u8* base) {
  g_hidden_pointer = ctx.r3.u32;
  g_hidden_first_argument = ctx.r4.u64;
  const Twelve value = MakeTwelve(ctx.r4.u32);
  std::memcpy(base + g_hidden_pointer, &value, sizeof(value));
  ctx.r3.u64 = g_hidden_pointer;
}

void ReturnHiddenVector(PPCContext& ctx, u8* base) {
  g_hidden_pointer = ctx.r3.u32;
  g_hidden_first_argument = ctx.r4.u64;
  const PPCVRegister value = MakeVector(ctx.r4.u32);
  for (u32 i = 0; i < 16; ++i) {
    base[g_hidden_pointer + i] = value.u8[15 - i];
  }
  ctx.r3.u64 = g_hidden_pointer;
}

rex::ppc::ByValue<Twelve> HookTwelve(u32 first) {
  g_hidden_first_argument = first;
  return rex::ppc::ByValue<Twelve>{MakeTwelve(first)};
}

rex::ppc::ImportFunction<void(u32, u32, u32, u32, u32, u32, u32, rex::ppc::ByValue<Twelve>)>
    g_split_structure{ReadSplitStructure};
rex::ppc::ImportFunction<void(rex::ppc::ByValue<Four>, rex::ppc::ByValue<Twelve>)>
    g_justified_structures{ReadJustifiedStructures};
rex::ppc::ImportFunction<rex::ppc::ByValue<Twelve>(u32)> g_hidden_twelve{ReturnHiddenTwelve};
rex::ppc::ImportFunction<rex::ppc::ByValue<PPCVRegister>(u32)> g_hidden_vector{ReturnHiddenVector};

}  // namespace

REX_TEST_CASE("A 12 byte structure splits between r10 and the stack", "[kernel][call]") {
  GuestThread thread;
  g_split_structure(1, 2, 3, 4, 5, 6, 7, rex::ppc::ByValue<Twelve>{MakeTwelve(0x10)});
  CHECK(g_structure_registers[7] == ((u64{0x10} << 32) | 0x11));
  CHECK(g_structure_stack_word == 0x12);
}

REX_TEST_CASE("A 4 byte structure is right justified and a 12 byte one left justified",
              "[kernel][call]") {
  GuestThread thread;
  g_justified_structures(rex::ppc::ByValue<Four>{Four{0xAABB}},
                         rex::ppc::ByValue<Twelve>{MakeTwelve(0x20)});
  CHECK(g_structure_registers[0] == 0xAABB);
  CHECK(g_structure_registers[1] == ((u64{0x20} << 32) | 0x21));
  CHECK(g_structure_registers[2] == (u64{0x22} << 32));
}

REX_TEST_CASE("A structure over 8 bytes returns through a hidden pointer", "[kernel][call]") {
  GuestThread thread;
  const auto result = g_hidden_twelve(0x30);
  CHECK(g_hidden_pointer != 0);
  CHECK(g_hidden_first_argument == 0x30);
  CHECK(result.value.a == 0x30);
  CHECK(result.value.b == 0x31);
  CHECK(result.value.c == 0x32);
}

REX_TEST_CASE("A member function returning a vector uses the hidden pointer", "[kernel][call]") {
  GuestThread thread;
  const auto result = g_hidden_vector(5);
  CHECK(g_hidden_pointer != 0);
  CHECK(g_hidden_first_argument == 5);
  CHECK(SameVector(result.value, MakeVector(5)));
}

REX_TEST_CASE("A hook returning a structure over 8 bytes fills the caller's pointer",
              "[kernel][call]") {
  GuestThread thread;
  PPCContext& ctx = *thread.state.GetContext();
  u8* base = thread.state.GetAddressSpace()->GetVirtualMembase();
  const u32 destination = thread.stack + 0x100;
  ctx.r3.u64 = destination;
  ctx.r4.u64 = 0x40;
  rex::ppc::GuestToHostThunk<&HookTwelve>(ctx, base);
  CHECK(g_hidden_first_argument == 0x40);
  CHECK(ctx.r3.u32 == destination);
  CHECK(rex::memory::load<be_u32>(base + destination) == 0x40);
  CHECK(rex::memory::load<be_u32>(base + destination + 8) == 0x42);
}

namespace {

PPCContext* g_call_ctx = nullptr;
PPCContext* g_dpc_ctx = nullptr;
u32 g_drain_failures = 0;

void RecordingDPC(PPCContext& ctx, u8*) {
  g_dpc_ctx = &ctx;
  ctx.r3.u64 = 0;
}

void GuestDPC(PPCContext& ctx, u8* base) {
  const u32 dpc = ctx.r3.u32;
  g_call_ctx = &ctx;
  ctx.r1.u32 -= kFrameSize;
  const u32 frame = ctx.r1.u32;
  rex::memory::store<be_u32>(base + frame, kSentinel);
  ctx.r31.u64 = 0x3131;
  rex::test::Caller caller(ctx, base, rex::kernel::GetCurrent());
  caller.Kernel("KeInsertQueueDpc", {dpc, 0, 0});
  if (rex::memory::load<be_u32>(base + frame) != kSentinel || ctx.r1.u32 != frame ||
      ctx.r31.u64 != 0x3131) {
    ++g_drain_failures;
  }
  ctx.r1.u32 += kFrameSize;
}

rex::ppc::ImportFunction<void(u32)> g_guest_dpc{GuestDPC};

}  // namespace

REX_TEST_CASE("A DPC drained inside a call runs on the thread's register file", "[kernel][call]") {
  g_call_ctx = nullptr;
  g_dpc_ctx = nullptr;
  g_drain_failures = 0;
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([](rex::test::Caller& c) {
    auto* state = ThreadState::Get();
    const u32 r1 = state->GetContext()->r1.u32;
    const u32 thunk =
        StateOf(c.GetRuntime()).GetFunctionDispatcher()->AllocateThunk(&RecordingDPC, 0);
    CHECKED_ELSE(thunk != 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    const u32 dpc = c.Alloc(0x20);
    c.Kernel("KeInitializeDpc", {dpc, thunk, 0});
    g_guest_dpc(dpc);
    CHECK(g_dpc_ctx == state->GetContext());
    CHECK(g_call_ctx == state->GetContext());
    CHECK(g_drain_failures == 0);
    CHECK(state->GetContext()->r1.u32 == r1);
  });
}

namespace {

u32 g_main_fiber = 0;
u32 g_fiber_runs = 0;
u32 g_fiber_failures = 0;

void SwitchBackBody(PPCContext& ctx, u8* base) {
  ++g_fiber_runs;
  ctx.r3.u64 = g_main_fiber;
  rexshim_SwitchToFiber(ctx, base);
}

void GuestFiberBody(PPCContext& ctx, u8* base) {
  const u32 body = ctx.r3.u32;
  ctx.r1.u32 -= kFrameSize;
  const u32 frame = ctx.r1.u32;
  rex::memory::store<be_u32>(base + frame, kSentinel);
  ctx.r31.u64 = 0x3131;
  ctx.r3.u64 = 0;
  rexshim_ConvertThreadToFiber(ctx, base);
  g_main_fiber = ctx.r3.u32;
  ctx.r3.u64 = 0;
  ctx.r4.u64 = body;
  ctx.r5.u64 = 0;
  rexshim_CreateFiber(ctx, base);
  const u32 fiber = ctx.r3.u32;
  ctx.r3.u64 = fiber;
  rexshim_SwitchToFiber(ctx, base);
  if (rex::memory::load<be_u32>(base + frame) != kSentinel || ctx.r1.u32 != frame ||
      ctx.r31.u64 != 0x3131 || &ctx != ThreadState::Get()->GetContext()) {
    ++g_fiber_failures;
  }
  ctx.r3.u64 = fiber;
  rexshim_DeleteFiber(ctx, base);
  rexshim_ConvertFiberToThread(ctx, base);
  ctx.r1.u32 += kFrameSize;
}

rex::ppc::ImportFunction<void(u32)> g_guest_fiber{GuestFiberBody};

}  // namespace

REX_TEST_CASE("A fiber switch inside a call returns to the calling frame", "[kernel][call]") {
  g_main_fiber = 0;
  g_fiber_runs = 0;
  g_fiber_failures = 0;
  rex::test::KernelFixture fixture;
  fixture.RunOnGuestThread([](rex::test::Caller& c) {
    auto* state = ThreadState::Get();
    const u32 r1 = state->GetContext()->r1.u32;
    const u32 body =
        StateOf(c.GetRuntime()).GetFunctionDispatcher()->AllocateThunk(&SwitchBackBody, 0);
    CHECKED_ELSE(body != 0) {
      FAIL_CHECK("the check above failed");
      return;
    }
    g_guest_fiber(body);
    CHECK(g_fiber_runs == 1);
    CHECK(g_fiber_failures == 0);
    CHECK(state->GetContext()->r1.u32 == r1);
  });
}

namespace thread_frame_tests {

using rex::u32;

using rex::test::Caller;
using rex::test::Join;
using rex::test::KernelFixture;

namespace {

struct EntryFrame {
  u32 stack_base = 0;
  u32 r1 = 0;
  u32 back_chain = 0;
  u32 root_nonzero_words = 0;
};

}  // namespace

REX_TEST_CASE("A thread entry runs in a call frame under a zeroed root frame",
              "[kernel][threads][frame]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto frame = std::make_shared<EntryFrame>();
    const u32 handle = c.Spawn([frame](Caller& t) {
      const u32 kthread = t.Load32(t.GetContext().r13.u32 + 0x100);
      frame->stack_base = t.Load32(kthread + 0x5C);
      frame->r1 = t.GetContext().r1.u32;
      frame->back_chain = t.Load32(frame->r1);
      const u32 root = frame->stack_base - rex::ppc::kLinkageSize;
      for (u32 offset = 0; offset < rex::ppc::kLinkageSize; offset += 4) {
        frame->root_nonzero_words += t.Load32(root + offset) != 0;
      }
    });
    Join(c, handle);
    const u32 root = frame->stack_base - rex::ppc::kLinkageSize;
    CHECK(frame->back_chain == root);
    CHECK(frame->r1 == root - rex::ppc::kLinkageSize);
    CHECK(frame->root_nonzero_words == 0);
  });
}

}  // namespace thread_frame_tests
