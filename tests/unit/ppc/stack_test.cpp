/**
 * @file        tests/unit/ppc/stack_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

#include "rex_test.h"

#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/ppc/stack.h>
#include <rex/runtime.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xmemory.h>

#include "../kernel/kernel_fixture.h"
#include "crash_child_runner.h"

namespace {

alignas(64) u8 g_mem[0x10000] = {};

struct Pair {
  be_u32 a;
  be_u16 b;
};

enum class Mode : u32 { kWide = 0x01020304 };
enum class Small : u16 { kPair = 0x0A0B };

}  // namespace

REX_TEST_CASE("StackFrame restores r1 at scope exit", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  {
    rex::ppc::StackFrame frame(ctx, g_mem);
    frame.Push<u32>(1);
    frame.PushString("temporary");
    frame.Push<u64>();
    CHECK(ctx.r1.u32 < 0x8000);
  }

  CHECK(ctx.r1.u32 == 0x8000);
}

REX_TEST_CASE("Push of an arithmetic value stores it big endian", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  rex::ppc::StackFrame frame(ctx, g_mem);

  auto value = frame.Push<u32>(0x11223344);

  CHECK(value.GetGuestAddress() == ctx.r1.u32);
  CHECK(*value.As<be_u32*>() == 0x11223344u);
  const u8 expected[4] = {0x11, 0x22, 0x33, 0x44};
  CHECK(std::memcmp(g_mem + value.GetGuestAddress(), expected, 4) == 0);
}

REX_TEST_CASE("Push of a struct copies its be_ fields", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  rex::ppc::StackFrame frame(ctx, g_mem);

  auto pair = frame.Push(Pair{be_u32(0xAABBCCDDu), be_u16(u16{0x1122})});

  CHECK(pair->a == 0xAABBCCDDu);
  CHECK(pair->b == 0x1122u);
  const u8 expected[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22};
  CHECK(std::memcmp(g_mem + pair.GetGuestAddress(), expected, 6) == 0);
}

REX_TEST_CASE("Push of an enum stores it big endian", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  rex::ppc::StackFrame frame(ctx, g_mem);

  auto wide = frame.Push(Mode::kWide);
  auto small = frame.Push(Small::kPair);

  CHECK(*wide.As<be_u32*>() == 0x01020304u);
  const u8 expected_wide[4] = {1, 2, 3, 4};
  CHECK(std::memcmp(g_mem + wide.GetGuestAddress(), expected_wide, 4) == 0);
  CHECK(*small.As<be_u16*>() == 0x0A0Bu);
  const u8 expected_small[2] = {0x0A, 0x0B};
  CHECK(std::memcmp(g_mem + small.GetGuestAddress(), expected_small, 2) == 0);
}

REX_TEST_CASE("Push of a float stores its bits big endian", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  rex::ppc::StackFrame frame(ctx, g_mem);

  const f32 single_value = std::bit_cast<f32>(0x40490FDBu);
  auto single = frame.Push(single_value);
  const u8 expected_single[4] = {0x40, 0x49, 0x0F, 0xDB};
  CHECK(std::memcmp(g_mem + single.GetGuestAddress(), expected_single, 4) == 0);
  CHECK(static_cast<f32>(*single.As<be_f32*>()) == single_value);

  const f64 double_value = std::bit_cast<f64>(0x400921FB54442D18ull);
  auto wide = frame.Push(double_value);
  const u8 expected_double[8] = {0x40, 0x09, 0x21, 0xFB, 0x54, 0x44, 0x2D, 0x18};
  CHECK(std::memcmp(g_mem + wide.GetGuestAddress(), expected_double, 8) == 0);
  CHECK(static_cast<f64>(*wide.As<be_f64*>()) == double_value);
}

REX_TEST_CASE("Push without a value is zeroed", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  std::memset(g_mem + 0x7000, 0xFF, 0x1000);
  rex::ppc::StackFrame frame(ctx, g_mem);

  auto block = frame.Push<std::array<u8, 64>>();

  for (u8 byte : *block) {
    CHECK(byte == 0);
  }
}

REX_TEST_CASE("PushString and PushWideString terminate", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  std::memset(g_mem + 0x7000, 0xFF, 0x1000);
  rex::ppc::StackFrame frame(ctx, g_mem);

  auto text = frame.PushString("PREFIX");
  const u8 expected_text[7] = {'P', 'R', 'E', 'F', 'I', 'X', 0};
  CHECK(std::memcmp(g_mem + text.GetGuestAddress(), expected_text, 7) == 0);

  auto wide = frame.PushWideString(u"ab");
  CHECK(*(wide + 0) == 0x61u);
  CHECK(*(wide + 1) == 0x62u);
  CHECK(*(wide + 2) == 0u);
  const u8 expected_wide[6] = {0, 0x61, 0, 0x62, 0, 0};
  CHECK(std::memcmp(g_mem + wide.GetGuestAddress(), expected_wide, 6) == 0);
}

REX_TEST_CASE("PushBytes copies the bytes as they are", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  rex::ppc::StackFrame frame(ctx, g_mem);

  const std::array<u8, 3> data = {0xAA, 0xBB, 0xCC};
  auto bytes = frame.PushBytes(data);

  CHECK(std::memcmp(g_mem + bytes.GetGuestAddress(), data.data(), 3) == 0);
}

REX_TEST_CASE("Pushes are 16 byte aligned", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  rex::ppc::StackFrame frame(ctx, g_mem);

  CHECK(frame.Push<u8>(1).GetGuestAddress() % 16 == 0);
  CHECK(frame.Push<u16>(2).GetGuestAddress() % 16 == 0);
  CHECK(frame.Push<f64>(3.0).GetGuestAddress() % 16 == 0);
  CHECK(frame.PushString("five!").GetGuestAddress() % 16 == 0);
  CHECK(frame.PushWideString(u"odd").GetGuestAddress() % 16 == 0);
  CHECK(frame.PushBytes(std::array<u8, 3>{}).GetGuestAddress() % 16 == 0);
  CHECK(ctx.r1.u32 % 16 == 0);
}

REX_TEST_CASE("Pushes get distinct addresses", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  rex::ppc::StackFrame frame(ctx, g_mem);

  auto first = frame.PushString("first");
  auto second = frame.PushString("second");

  CHECK(first.GetGuestAddress() != second.GetGuestAddress());
  CHECK(std::strcmp(first.GetHostAddress(), "first") == 0);
  CHECK(std::strcmp(second.GetHostAddress(), "second") == 0);
}

REX_TEST_CASE("Nested frames restore in order", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  {
    rex::ppc::StackFrame outer(ctx, g_mem);
    outer.PushString("outer");
    const u32 outer_r1 = ctx.r1.u32;

    {
      rex::ppc::StackFrame inner(ctx, g_mem);
      inner.PushString("inner");
      CHECK(ctx.r1.u32 < outer_r1);
    }

    CHECK(ctx.r1.u32 == outer_r1);
  }

  CHECK(ctx.r1.u32 == 0x8000);
}

#ifndef NDEBUG
REX_TEST_CASE("Closing frames out of order asserts in Debug", "[ppc][stack][crash_report]") {
  const auto result = rex::test::RunChild("stack-frames-out-of-order");
  CHECK(result.status != 0);
  CHECK(result.Has("StackFrame closed out of order"));
  CHECK_FALSE(result.Has("stack frames closed out of order"));
}

REX_TEST_CASE("A push below the stack limit aborts", "[ppc][stack][crash_report]") {
  const auto result = rex::test::RunChild("stack-push-past-limit");
  CHECK(result.status != 0);
  CHECK(result.Has("pushed above the limit"));
  CHECK(result.Has("PPC stack overflow"));
}

REX_TEST_CASE("PushBytes of an empty span asserts in Debug", "[ppc][stack][crash_report]") {
  const auto result = rex::test::RunChild("stack-push-empty-bytes");
  CHECK(result.status != 0);
  CHECK(result.Has("PushBytes needs at least 1 byte"));
}
#endif

REX_TEST_CASE("A MappedPtr import argument passes the guest address", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  rex::ppc::StackFrame frame(ctx, g_mem);
  auto value = frame.Push<u32>(7);

  rex::ppc::ArgTranslator::SetValue(ctx, g_mem, rex::ppc::ArgPlace{.slot = 0, .slots = 1, .gpr = 0},
                                    value);

  CHECK(ctx.r3.u32 == value.GetGuestAddress());
}

REX_TEST_CASE("A default StackFrame binds the context of the calling thread", "[ppc][stack]") {
  rex::test::KernelFixture fixture;
  rex::runtime::Memory& memory = *fixture.GetRuntime().GetMemory();
  rex::runtime::BaseHeap* heap = memory.LookupHeapByType(true, 4096);
  u32 block = 0;
  REQUIRE(heap->Alloc(
      0x2000, 0x1000,
      rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &block));
  rex::runtime::ThreadState state(0, block + 0x2000, 0, &memory, &fixture.GetRuntime());
  rex::runtime::ThreadState* previous = rex::runtime::ThreadState::Get();
  rex::runtime::ThreadState::Bind(&state);

  {
    rex::ppc::StackFrame frame;
    CHECK(&frame.GetContext() == state.GetContext());
    CHECK(frame.GetBase() == memory.GetVirtualMembase());
    frame.Push<u32>();
    CHECK(state.GetContext()->r1.u32 < block + 0x2000);
  }

  CHECK(state.GetContext()->r1.u32 == block + 0x2000);
  rex::runtime::ThreadState::Bind(previous);
  CHECK(heap->Release(block));
}

REX_TEST_CASE("StackFrame pushes and the PCR read reach the 0xE0 heap through its page offset",
              "[ppc][stack]") {
  rex::test::KernelFixture fixture;
  rex::runtime::Memory& memory = *fixture.GetRuntime().GetMemory();
  rex::runtime::BaseHeap* heap = memory.LookupHeapByType(true, 4096);
  u32 block = 0;
  REQUIRE(heap->Alloc(
      0x2000, 0x1000,
      rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
      rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite, false, &block));
  REQUIRE(block >= 0xE0000000);
  u8* base = memory.GetVirtualMembase();

  *memory.TranslateVirtual<be_u32*>(block + 0x74) = block;
  PPCContext ctx{};
  ctx.r13.u32 = block;
  CHECK(rex::ppc::GetStackLimit(ctx, base) == block);

  ctx.r1.u32 = block + 0x2000;
  {
    rex::ppc::StackFrame frame(ctx, base);

    const auto value = frame.Push<u32>(0x12345678);
    CHECK(*memory.TranslateVirtual<be_u32*>(value.GetGuestAddress()) == 0x12345678u);

    const std::array<u8, 4> bytes = {'a', 'b', 'c', 0};
    const auto raw = frame.PushBytes(bytes);
    CHECK(std::memcmp(memory.TranslateVirtual(raw.GetGuestAddress()), "abc", 4) == 0);

    const auto text = frame.PushString("xyz");
    CHECK(std::strcmp(memory.TranslateVirtual<const char*>(text.GetGuestAddress()), "xyz") == 0);
  }
  CHECK(ctx.r1.u32 == block + 0x2000);
  CHECK(heap->Release(block));
}
