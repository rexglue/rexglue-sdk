/**
 * @file        tests/unit/ppc/import_function_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/context.h>
#include <rex/ppc/function.h>

using namespace rex::ppc;

alignas(64) static u8 g_test_mem[0x10000] = {};

TEST_CASE("SetIntegerArgumentValue writes stack args for index > 7", "[ppc][arg_translator]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_mem, 8, 0xDEADBEEF);

  u64 readback = ArgTranslator::GetIntegerArgumentValue(ctx, g_test_mem, 8);
  CHECK(readback == 0xDEADBEEF);
}

TEST_CASE("SetIntegerArgumentValue writes multiple stack args at correct offsets",
          "[ppc][arg_translator]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  std::memset(g_test_mem + 0x8000, 0, 0x100);

  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_mem, 8, 0x11111111);
  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_mem, 9, 0x22222222);
  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_mem, 10, 0x33333333);

  CHECK(ArgTranslator::GetIntegerArgumentValue(ctx, g_test_mem, 8) == 0x11111111);
  CHECK(ArgTranslator::GetIntegerArgumentValue(ctx, g_test_mem, 9) == 0x22222222);
  CHECK(ArgTranslator::GetIntegerArgumentValue(ctx, g_test_mem, 10) == 0x33333333);
}

TEST_CASE("SetIntegerArgumentValue still works for register args 0-7", "[ppc][arg_translator]") {
  PPCContext ctx{};
  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_mem, 0, 0xAAAA);
  ArgTranslator::SetIntegerArgumentValue(ctx, g_test_mem, 7, 0xBBBB);
  CHECK(ctx.r3.u64 == 0xAAAA);
  CHECK(ctx.r10.u64 == 0xBBBB);
}

TEST_CASE("A u32 argument reads the low word of a sign extended register",
          "[ppc][arg_translator]") {
  PPCContext ctx{};
  ctx.r3.u64 = 0xFFFFFFFF00000010ull;
  CHECK(ArgTranslator::GetValue<u32>(ctx, g_test_mem, rex::ppc::Argument{}) == 0x10);
}

TEST_CASE("A 64-bit argument reads its whole register and stack slot", "[ppc][arg_translator]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  ctx.r4.u64 = 0x0123456789ABCDEFull;
  const u64 stacked = 0xFEDCBA9876543210ull;
  for (i32 i = 0; i < 8; ++i) {
    g_test_mem[0x8050 + i] = static_cast<u8>(stacked >> (56 - i * 8));
  }
  CHECK(ArgTranslator::GetValue<u64>(ctx, g_test_mem, Argument{0, 1, 1}) == 0x0123456789ABCDEFull);
  CHECK(ArgTranslator::GetValue<u64>(ctx, g_test_mem, Argument{0, 8, 8}) == stacked);
}

TEST_CASE("A bool argument reads only the low word of its register", "[ppc][arg_translator]") {
  PPCContext ctx{};
  ctx.r3.u64 = 0xFFFFFFFF00000000ull;
  CHECK_FALSE(ArgTranslator::GetValue<bool>(ctx, g_test_mem, Argument{0, 0, 0}));
}
