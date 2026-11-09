/**
 * @file        tests/unit/ppc/arg_translator_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <cstring>

#include "rex_test.h"

#include <rex/memory/mapped_ptr.h>
#include <rex/ppc/context.h>
#include <rex/ppc/call.h>

using namespace rex::ppc;

alignas(64) static u8 g_test_memory[0x10000] = {};

REX_TEST_CASE("SetIntegerArgumentValue writes stack args for index > 7", "[ppc][arg_translator]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_memory, 8, 0xDEADBEEF);

  u64 readback = ArgTranslator::GetIntegerArgumentValue(ctx, g_test_memory, 8);
  CHECK(readback == 0xDEADBEEF);
}

REX_TEST_CASE("SetIntegerArgumentValue writes multiple stack arguments at correct offsets",
              "[ppc][arg_translator]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  std::memset(g_test_memory + 0x8000, 0, 0x100);

  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_memory, 8, 0x11111111);
  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_memory, 9, 0x22222222);
  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_memory, 10, 0x33333333);

  CHECK(ArgTranslator::GetIntegerArgumentValue(ctx, g_test_memory, 8) == 0x11111111);
  CHECK(ArgTranslator::GetIntegerArgumentValue(ctx, g_test_memory, 9) == 0x22222222);
  CHECK(ArgTranslator::GetIntegerArgumentValue(ctx, g_test_memory, 10) == 0x33333333);
}

REX_TEST_CASE("SetIntegerArgumentValue works for register arguments 0 to 7",
              "[ppc][arg_translator]") {
  PPCContext ctx{};
  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_memory, 0, 0xAAAA);
  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_memory, 7, 0xBBBB);
  CHECK(ctx.r3.u64 == 0xAAAA);
  CHECK(ctx.r10.u64 == 0xBBBB);
}

REX_TEST_CASE("A u32 argument reads the low word of a sign extended register",
              "[ppc][arg_translator]") {
  PPCContext ctx{};
  ctx.r3.u64 = 0xFFFFFFFF00000010ull;
  CHECK(ArgTranslator::GetValue<u32>(ctx, g_test_memory,
                                     ArgPlace{.slot = 0, .slots = 1, .gpr = 0}) == 0x10);
}

REX_TEST_CASE("A 64-bit argument reads its whole register and stack slot",
              "[ppc][arg_translator]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  ctx.r4.u64 = 0x0123456789ABCDEFull;
  const u64 stacked = 0xFEDCBA9876543210ull;
  for (i32 i = 0; i < 8; ++i) {
    g_test_memory[0x8050 + i] = static_cast<u8>(stacked >> (56 - i * 8));
  }
  CHECK(
      ArgTranslator::GetValue<u64>(ctx, g_test_memory, ArgPlace{.slot = 1, .slots = 1, .gpr = 1}) ==
      0x0123456789ABCDEFull);
  CHECK(ArgTranslator::GetValue<u64>(ctx, g_test_memory,
                                     ArgPlace{.slot = 8, .slots = 1, .stack = true}) == stacked);
}

REX_TEST_CASE("A bool argument reads only the low word of its register", "[ppc][arg_translator]") {
  PPCContext ctx{};
  ctx.r3.u64 = 0xFFFFFFFF00000000ull;
  CHECK_FALSE(
      ArgTranslator::GetValue<bool>(ctx, g_test_memory, ArgPlace{.slot = 0, .slots = 1, .gpr = 0}));
}

namespace {

u32* g_returned_pointer = nullptr;

u32* ReturnPointer() {
  return g_returned_pointer;
}

u32* AtGuest(u32 guest_address) {
  const uintptr_t host_offset = static_cast<uintptr_t>(guest_address) +
                                rex::memory::detail::PhysicalHostOffset(guest_address);
  return reinterpret_cast<u32*>(g_test_memory + host_offset);
}

}  // namespace

REX_TEST_CASE("A host pointer argument is written as its guest address", "[ppc][arg_translator]") {
  const ArgPlace arg{.slot = 0, .slots = 1, .gpr = 0};
  for (const u32 guest :
       {0x00010000u, 0x7FFFFFF0u, 0xDFFFFFFCu, 0xE0000000u, 0xE0000010u, 0xFFFFFFFCu}) {
    INFO(guest);
    PPCContext ctx{};
    ArgTranslator::SetValue(ctx, g_test_memory, arg, AtGuest(guest));
    CHECK(ctx.r3.u32 == guest);
    CHECK(ArgTranslator::GetValue<u32*>(ctx, g_test_memory, arg) == AtGuest(guest));
  }
}

REX_TEST_CASE("A null host pointer argument is written as 0", "[ppc][arg_translator]") {
  const ArgPlace arg{.slot = 0, .slots = 1, .gpr = 0};
  PPCContext ctx{};
  ctx.r3.u64 = 0x1234;
  ArgTranslator::SetValue(ctx, g_test_memory, arg, static_cast<u32*>(nullptr));
  CHECK(ctx.r3.u64 == 0);
  ctx.r3.u64 = 0x1234;
  ArgTranslator::SetValue(ctx, g_test_memory, arg, nullptr);
  CHECK(ctx.r3.u64 == 0);
}

REX_TEST_CASE("A pointer result is returned as its guest address", "[ppc][arg_translator]") {
  for (const u32 guest : {0x00010000u, 0xDFFFFFFCu, 0xE0000000u, 0xE0000010u, 0xFFFFFFFCu}) {
    INFO(guest);
    PPCContext ctx{};
    g_returned_pointer = AtGuest(guest);
    GuestToHostThunk<&ReturnPointer>(ctx, g_test_memory);
    CHECK(ctx.r3.u32 == guest);
  }
  PPCContext ctx{};
  ctx.r3.u64 = 0x1234;
  g_returned_pointer = nullptr;
  GuestToHostThunk<&ReturnPointer>(ctx, g_test_memory);
  CHECK(ctx.r3.u64 == 0);
}

REX_TEST_CASE("A register value converts to a host pointer by its low word",
              "[ppc][arg_translator]") {
  for (const u32 guest : {0x00010000u, 0x7FFFFFF0u, 0xDFFFFFFCu, 0xE0000000u, 0xE0000010u}) {
    INFO(guest);
    CHECK(ArgTranslator::GetPointerValue<u32*>(guest, g_test_memory) == AtGuest(guest));
    CHECK(ArgTranslator::GetPointerValue<u32*>(0xABCD000000000000ull | guest, g_test_memory) ==
          AtGuest(guest));
    CHECK(ArgTranslator::GetPointerValue<void*>(guest, g_test_memory) ==
          static_cast<void*>(AtGuest(guest)));
  }
  CHECK(ArgTranslator::GetPointerValue<u32*>(0, g_test_memory) == nullptr);
  CHECK(ArgTranslator::GetPointerValue<u32*>(0x1234000000000000ull, g_test_memory) == nullptr);
}

REX_TEST_CASE("A register value converts to a mapped pointer with its guest address",
              "[ppc][arg_translator]") {
  for (const u32 guest : {0x00010000u, 0xDFFFFFFCu, 0xE0000000u, 0xE0000010u}) {
    INFO(guest);
    const auto mapped =
        ArgTranslator::GetMappedPointerValue<rex::memory::MappedPtr<u32>>(guest, g_test_memory);
    CHECK(mapped.GetGuestAddress() == guest);
    CHECK(mapped.GetHostPtr() == AtGuest(guest));
  }
  const auto null =
      ArgTranslator::GetMappedPointerValue<rex::memory::MappedPtr<u32>>(0, g_test_memory);
  CHECK(null.GetGuestAddress() == 0);
  CHECK(null.GetHostPtr() == nullptr);
}

REX_TEST_CASE("A pointer argument equals the converted register value", "[ppc][arg_translator]") {
  const ArgPlace arg{.slot = 0, .slots = 1, .gpr = 0};
  PPCContext ctx{};
  ctx.r3.u64 = 0x00020040;
  CHECK(ArgTranslator::GetValue<u32*>(ctx, g_test_memory, arg) ==
        ArgTranslator::GetPointerValue<u32*>(ctx.r3.u64, g_test_memory));
}
