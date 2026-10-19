/**
 * @file        tests/unit/ppc/stack_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <bit>
#include <cstdint>
#include <cstring>

#include "rex_test.h"

#include <rex/ppc/context.h>
#include <rex/ppc/stack.h>
#include <rex/runtime.h>
#include <rex/runtime/xmemory.h>

#include "../kernel/kernel_fixture.h"

alignas(64) static u8 g_mem[0x10000] = {};

REX_TEST_CASE("stack_push scalar decrements r1 and writes byte swapped value", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  u32 addr = rex::ppc::stack_push(ctx, g_mem, u32{0x12345678});

  CHECK(ctx.r1.u32 == 0x8000 - 16);
  CHECK(addr == ctx.r1.u32);
  u32 raw;
  std::memcpy(&raw, g_mem + addr, 4);
  CHECK(raw == std::byteswap(u32{0x12345678}));
}

REX_TEST_CASE("stack_push float writes byte swapped float", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  f32 val = 3.14f;
  u32 addr = rex::ppc::stack_push(ctx, g_mem, val);

  CHECK(ctx.r1.u32 == 0x8000 - 16);
  u32 raw;
  std::memcpy(&raw, g_mem + addr, 4);
  u32 val_bits;
  std::memcpy(&val_bits, &val, 4);
  CHECK(raw == std::byteswap(val_bits));
}

REX_TEST_CASE("stack_push_string writes NUL terminated string", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  std::memset(g_mem + 0x7F00, 0, 0x100);

  u32 addr = rex::ppc::stack_push_string(ctx, g_mem, "hello");

  CHECK(ctx.r1.u32 == 0x8000 - 16);
  CHECK(addr == ctx.r1.u32);
  CHECK(std::strcmp(reinterpret_cast<char*>(g_mem + addr), "hello") == 0);
}

REX_TEST_CASE("stack_push_string aligns to 16 bytes", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  u32 addr = rex::ppc::stack_push_string(ctx, g_mem, "longstring12345678");
  CHECK(ctx.r1.u32 == 0x8000 - 32);
  CHECK(addr == ctx.r1.u32);
}

REX_TEST_CASE("stack_push keeps r1 16 byte aligned", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  rex::ppc::stack_push(ctx, g_mem, u8{1});
  CHECK(ctx.r1.u32 % 16 == 0);
  rex::ppc::stack_push(ctx, g_mem, u16{2});
  CHECK(ctx.r1.u32 % 16 == 0);
  rex::ppc::stack_push(ctx, g_mem, f64{3.0});
  CHECK(ctx.r1.u32 % 16 == 0);
  rex::ppc::stack_push_string(ctx, g_mem, "five!");
  CHECK(ctx.r1.u32 % 16 == 0);
}

REX_TEST_CASE("stack_push raw bytes", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  u8 data[] = {0xAA, 0xBB, 0xCC};
  u32 addr = rex::ppc::stack_push(ctx, g_mem, data, 3);

  CHECK(ctx.r1.u32 == 0x8000 - 16);
  CHECK(g_mem[addr] == 0xAA);
  CHECK(g_mem[addr + 1] == 0xBB);
  CHECK(g_mem[addr + 2] == 0xCC);
}

REX_TEST_CASE("stack_pop restores r1", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  rex::ppc::stack_push(ctx, g_mem, u32{42});
  CHECK(ctx.r1.u32 == 0x8000 - 16);

  rex::ppc::stack_pop(ctx, 8);
  CHECK(ctx.r1.u32 == 0x8000);
}

REX_TEST_CASE("multiple pushes return stable addresses", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  u32 addr1 = rex::ppc::stack_push_string(ctx, g_mem, "first");
  u32 addr2 = rex::ppc::stack_push_string(ctx, g_mem, "second");

  CHECK(addr1 != addr2);
  CHECK(std::strcmp(reinterpret_cast<char*>(g_mem + addr1), "first") == 0);
  CHECK(std::strcmp(reinterpret_cast<char*>(g_mem + addr2), "second") == 0);
}

REX_TEST_CASE("stack_guard restores r1 on scope exit", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  {
    rex::ppc::StackGuard guard(ctx);
    rex::ppc::stack_push_string(ctx, g_mem, "temporary");
    CHECK(ctx.r1.u32 < 0x8000);
  }

  CHECK(ctx.r1.u32 == 0x8000);
}

REX_TEST_CASE("stack_guard restores after multiple pushes", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  {
    rex::ppc::StackGuard guard(ctx);
    rex::ppc::stack_push(ctx, g_mem, u32{1});
    rex::ppc::stack_push(ctx, g_mem, u32{2});
    rex::ppc::stack_push_string(ctx, g_mem, "three");
    CHECK(ctx.r1.u32 == 0x8000 - 16 - 16 - 16);
  }

  CHECK(ctx.r1.u32 == 0x8000);
}

REX_TEST_CASE("nested stack_guards restore correctly", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  {
    rex::ppc::StackGuard outer(ctx);
    rex::ppc::stack_push_string(ctx, g_mem, "outer");

    {
      rex::ppc::StackGuard inner(ctx);
      rex::ppc::stack_push_string(ctx, g_mem, "inner");
    }
    CHECK(ctx.r1.u32 == 0x8000 - 16);
  }
  CHECK(ctx.r1.u32 == 0x8000);
}

REX_TEST_CASE("stack_push and the PCR read reach the 0xE0 heap through its page offset",
              "[ppc][stack]") {
  rex::testing::KernelFixture fixture;
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
  CHECK(rex::ppc::stack_limit_from_pcr(ctx, base) == block);

  ctx.r1.u32 = block + 0x2000;
  const u32 addr = rex::ppc::stack_push(ctx, base, u32{0x12345678});
  CHECK(*memory.TranslateVirtual<be_u32*>(addr) == 0x12345678u);

  const u32 raw = rex::ppc::stack_push(ctx, base, "abc", 4);
  CHECK(std::memcmp(memory.TranslateVirtual(raw), "abc", 4) == 0);

  const u32 str = rex::ppc::stack_push_string(ctx, base, "xyz");
  CHECK(std::strcmp(memory.TranslateVirtual<const char*>(str), "xyz") == 0);
  CHECK(heap->Release(block));
}
