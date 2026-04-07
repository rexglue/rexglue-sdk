/**
 * Tests for rex::ppc stack push/pop/guard operations.
 */

#include <cstdint>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/context.h>
#include <rex/ppc/stack.h>

// 64KB test memory, aligned
alignas(64) static uint8_t g_mem[0x10000] = {};

TEST_CASE("stack_push scalar decrements r1 and writes byte-swapped value", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  uint32_t addr = rex::ppc::stack_push(ctx, g_mem, uint32_t{0x12345678});

  CHECK(ctx.r1.u32 == 0x8000 - 8);
  CHECK(addr == ctx.r1.u32);
  uint32_t raw;
  std::memcpy(&raw, g_mem + addr, 4);
  CHECK(raw == __builtin_bswap32(0x12345678));
}

TEST_CASE("stack_push float writes byte-swapped float", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  float val = 3.14f;
  uint32_t addr = rex::ppc::stack_push(ctx, g_mem, val);

  CHECK(ctx.r1.u32 == 0x8000 - 8);
  uint32_t raw;
  std::memcpy(&raw, g_mem + addr, 4);
  uint32_t val_bits;
  std::memcpy(&val_bits, &val, 4);
  CHECK(raw == __builtin_bswap32(val_bits));
}

TEST_CASE("stack_push_string writes NUL-terminated string", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;
  std::memset(g_mem + 0x7F00, 0, 0x100);

  uint32_t addr = rex::ppc::stack_push_string(ctx, g_mem, "hello");

  CHECK(ctx.r1.u32 == 0x8000 - 8);
  CHECK(addr == ctx.r1.u32);
  CHECK(std::strcmp(reinterpret_cast<char*>(g_mem + addr), "hello") == 0);
}

TEST_CASE("stack_push_string aligns to 8 bytes", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  uint32_t addr = rex::ppc::stack_push_string(ctx, g_mem, "longstring1");
  CHECK(ctx.r1.u32 == 0x8000 - 16);
  CHECK(addr == ctx.r1.u32);
}

TEST_CASE("stack_push raw bytes", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  uint8_t data[] = {0xAA, 0xBB, 0xCC};
  uint32_t addr = rex::ppc::stack_push(ctx, g_mem, data, 3);

  CHECK(ctx.r1.u32 == 0x8000 - 8);
  CHECK(g_mem[addr] == 0xAA);
  CHECK(g_mem[addr + 1] == 0xBB);
  CHECK(g_mem[addr + 2] == 0xCC);
}

TEST_CASE("stack_pop restores r1", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  rex::ppc::stack_push(ctx, g_mem, uint32_t{42});
  CHECK(ctx.r1.u32 == 0x8000 - 8);

  rex::ppc::stack_pop(ctx, 8);
  CHECK(ctx.r1.u32 == 0x8000);
}

TEST_CASE("multiple pushes return stable addresses", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  uint32_t addr1 = rex::ppc::stack_push_string(ctx, g_mem, "first");
  uint32_t addr2 = rex::ppc::stack_push_string(ctx, g_mem, "second");

  CHECK(addr1 != addr2);
  CHECK(std::strcmp(reinterpret_cast<char*>(g_mem + addr1), "first") == 0);
  CHECK(std::strcmp(reinterpret_cast<char*>(g_mem + addr2), "second") == 0);
}

TEST_CASE("stack_guard restores r1 on scope exit", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  {
    rex::ppc::stack_guard guard(ctx);
    rex::ppc::stack_push_string(ctx, g_mem, "temporary");
    CHECK(ctx.r1.u32 < 0x8000);
  }

  CHECK(ctx.r1.u32 == 0x8000);
}

TEST_CASE("stack_guard restores after multiple pushes", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  {
    rex::ppc::stack_guard guard(ctx);
    rex::ppc::stack_push(ctx, g_mem, uint32_t{1});
    rex::ppc::stack_push(ctx, g_mem, uint32_t{2});
    rex::ppc::stack_push_string(ctx, g_mem, "three");
    CHECK(ctx.r1.u32 == 0x8000 - 8 - 8 - 8);
  }

  CHECK(ctx.r1.u32 == 0x8000);
}

TEST_CASE("nested stack_guards restore correctly", "[ppc][stack]") {
  PPCContext ctx{};
  ctx.r1.u32 = 0x8000;

  {
    rex::ppc::stack_guard outer(ctx);
    rex::ppc::stack_push_string(ctx, g_mem, "outer");

    {
      rex::ppc::stack_guard inner(ctx);
      rex::ppc::stack_push_string(ctx, g_mem, "inner");
    }
    CHECK(ctx.r1.u32 == 0x8000 - 8);
  }
  CHECK(ctx.r1.u32 == 0x8000);
}
