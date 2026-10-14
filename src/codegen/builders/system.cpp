/**
 * @file        codegen/builders/system.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "helpers.h"

namespace rex::codegen {

bool BuildNop(BuilderContext& ctx) {
  (void)ctx;
  return true;
}

bool BuildAttn(BuilderContext& ctx) {
  // Xenon debug breakpoint, no effect in recompiled code
  (void)ctx;
  return true;
}

bool BuildSync(BuilderContext& ctx) {
  ctx.Println("\tstd::atomic_thread_fence(std::memory_order_seq_cst);");
  return true;
}

bool BuildLwsync(BuilderContext& ctx) {
  ctx.Println("\tstd::atomic_thread_fence(std::memory_order_acq_rel);");
  return true;
}

bool BuildEieio(BuilderContext& ctx) {
  ctx.Println("\tstd::atomic_thread_fence(std::memory_order_seq_cst);");
  return true;
}

bool BuildDb16cyc(BuilderContext& ctx) {
  // Xenon 16-cycle delay hint, no effect in recompiled code
  (void)ctx;
  return true;
}

bool BuildCctpl(BuilderContext& ctx) {
  // Xenon cache control thread priority low, no effect in recompiled code
  (void)ctx;
  return true;
}

bool BuildCctpm(BuilderContext& ctx) {
  // Xenon cache control thread priority medium, no effect in recompiled code
  (void)ctx;
  return true;
}

bool BuildCctph(BuilderContext& ctx) {
  // Xenon cache control thread priority high, no effect in recompiled code
  (void)ctx;
  return true;
}

bool BuildTdi(BuilderContext& ctx) {
  u32 to = (ctx.insn.instruction >> 21) & 0x1F;
  u32 ra = (ctx.insn.instruction >> 16) & 0x1F;
  i64 simm = static_cast<i16>(ctx.insn.instruction & 0xFFFF);
  EmitTrap(ctx, to, std::format("{}.s64", ctx.R(ra)), std::format("{}.u64", ctx.R(ra)),
           std::format("{}ll", simm), std::format("{}ull", static_cast<u64>(simm)));
  return true;
}

bool BuildTwi(BuilderContext& ctx) {
  u32 to = (ctx.insn.instruction >> 21) & 0x1F;
  u32 ra = (ctx.insn.instruction >> 16) & 0x1F;
  i32 simm = static_cast<i16>(ctx.insn.instruction & 0xFFFF);

  // twi 31, r0, <imm> is an unconditional trap with service code in the immediate
  if (to == 0x1F && ra == 0) {
    u16 trap_type = static_cast<u16>(simm);
    ctx.Println("\tppc_trap(ctx, base, {});", trap_type);
    return true;
  }

  EmitTrap(ctx, to, std::format("{}.s32", ctx.R(ra)), std::format("{}.u32", ctx.R(ra)),
           std::format("{}", simm), std::format("{}u", static_cast<u32>(simm)));
  return true;
}

bool BuildTd(BuilderContext& ctx) {
  u32 to = (ctx.insn.instruction >> 21) & 0x1F;
  u32 ra = (ctx.insn.instruction >> 16) & 0x1F;
  u32 rb = (ctx.insn.instruction >> 11) & 0x1F;
  EmitTrap(ctx, to, std::format("{}.s64", ctx.R(ra)), std::format("{}.u64", ctx.R(ra)),
           std::format("{}.s64", ctx.R(rb)), std::format("{}.u64", ctx.R(rb)));
  return true;
}

bool BuildTw(BuilderContext& ctx) {
  u32 to = (ctx.insn.instruction >> 21) & 0x1F;
  u32 ra = (ctx.insn.instruction >> 16) & 0x1F;
  u32 rb = (ctx.insn.instruction >> 11) & 0x1F;
  EmitTrap(ctx, to, std::format("{}.s32", ctx.R(ra)), std::format("{}.u32", ctx.R(ra)),
           std::format("{}.s32", ctx.R(rb)), std::format("{}.u32", ctx.R(rb)));
  return true;
}

bool BuildDcbf(BuilderContext& ctx) {
  // Hint instruction, access violation callback handlers take care of this on write
  (void)ctx;
  return true;
}

bool BuildDcbt(BuilderContext& ctx) {
  // Hint instruction, prefetch has no semantic effect
  (void)ctx;
  return true;
}

bool BuildDcbtst(BuilderContext& ctx) {
  // Hint instruction, prefetch for store has no semantic effect
  (void)ctx;
  return true;
}

bool BuildDcbz(BuilderContext& ctx) {
  ctx.Print("\t{} = (", ctx.EA());
  if (ctx.insn.operands[0] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[0]));
  ctx.Println("{}.u32) & ~31;", ctx.R(ctx.insn.operands[1]));
  ctx.Println("\tmemset((void*)REX_RAW_ADDR({}), 0, 32);", ctx.EA());
  return true;
}

bool BuildDcbzl(BuilderContext& ctx) {
  ctx.Print("\t{} = (", ctx.EA());
  if (ctx.insn.operands[0] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[0]));
  ctx.Println("{}.u32) & ~127;", ctx.R(ctx.insn.operands[1]));
  ctx.Println("\tmemset((void*)REX_RAW_ADDR({}), 0, 128);", ctx.EA());
  return true;
}

bool BuildDcbst(BuilderContext& ctx) {
  // Hint instruction, access violation callback handlers take care of this on write
  (void)ctx;
  return true;
}

bool BuildMr(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64;", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildMcrf(BuilderContext& ctx) {
  ctx.Println("\t{0} = {1};", ctx.CR(ctx.insn.operands[0]), ctx.CR(ctx.insn.operands[1]));
  return true;
}

bool BuildMfxer(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = ({}.so << 31) | ({}.ov << 30) | ({}.ca << 29);",
              ctx.R(ctx.insn.operands[0]), ctx.XER(), ctx.XER(), ctx.XER());
  return true;
}

bool BuildMfctr(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64;", ctx.R(ctx.insn.operands[0]), ctx.CTR());
  return true;
}

bool BuildMfcr(BuilderContext& ctx) {
  for (size_t i = 0; i < 32; i++) {
    constexpr std::string_view kFields[] = {"lt", "gt", "eq", "so"};
    ctx.Println("\t{}.u64 {}= {}.{} ? 0x{:X} : 0;", ctx.R(ctx.insn.operands[0]), i == 0 ? "" : "|",
                ctx.CR(i / 4), kFields[i % 4], 1u << (31 - i));
  }
  return true;
}

bool BuildMfocrf(BuilderContext& ctx) {
  // FXM is a one-hot mask with bit 7 for CR0, bit 6 for CR1, down to bit 0 for CR7
  u32 fxm = ctx.insn.operands[1];
  u32 cr_field = 0;
  for (u32 i = 0; i < 8; i++) {
    if (fxm & (0x80u >> i)) {
      cr_field = i;
      break;
    }
  }
  u32 base_shift = 28 - 4 * cr_field;
  ctx.Println("\t{}.u64 = ({}.lt << {}) | ({}.gt << {}) | ({}.eq << {}) | ({}.so << {});",
              ctx.R(ctx.insn.operands[0]), ctx.CR(cr_field), base_shift + 3, ctx.CR(cr_field),
              base_shift + 2, ctx.CR(cr_field), base_shift + 1, ctx.CR(cr_field), base_shift);
  return true;
}

bool BuildMflr(BuilderContext& ctx) {
  if (!ctx.GetConfig().skip_lr)
    ctx.Println("\t{}.u64 = ctx.lr;", ctx.R(ctx.insn.operands[0]));
  return true;
}

bool BuildMfmsr(BuilderContext& ctx) {
  if (!ctx.GetConfig().skip_msr) {
    // REX_CHECK_GLOBAL_LOCK yields 0x8000 when interrupts are enabled and 0 when locked
    ctx.Println("\t{}.u64 = REX_CHECK_GLOBAL_LOCK();", ctx.R(ctx.insn.operands[0]));
  }
  return true;
}

bool BuildMffs(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = ctx.fpscr.LoadFromHost();", ctx.F(ctx.insn.operands[0]));
  return true;
}

bool BuildMftb(BuilderContext& ctx) {
  // The timebase runs at 50 MHz
  ctx.Println("\t{}.u64 = REX_QUERY_TIMEBASE();", ctx.R(ctx.insn.operands[0]));
  return true;
}

bool BuildMftbu(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = REX_QUERY_TIMEBASE() >> 32;", ctx.R(ctx.insn.operands[0]));
  return true;
}

bool BuildMtcr(BuilderContext& ctx) {
  for (size_t i = 0; i < 32; i++) {
    constexpr std::string_view kFields[] = {"lt", "gt", "eq", "so"};
    ctx.Println("\t{}.{} = ({}.u32 & 0x{:X}) != 0;", ctx.CR(i / 4), kFields[i % 4],
                ctx.R(ctx.insn.operands[0]), 1u << (31 - i));
  }
  return true;
}

bool BuildMtcrf(BuilderContext& ctx) {
  u32 fxm = ctx.insn.operands[0];
  constexpr std::string_view kNames[] = {"lt", "gt", "eq", "so"};
  for (u32 field = 0; field < 8; field++) {
    if (fxm & (0x80u >> field)) {
      u32 base_bit = 28 - 4 * field;
      for (int b = 0; b < 4; b++) {
        ctx.Println("\t{}.{} = ({}.u32 & 0x{:X}) != 0;", ctx.CR(field), kNames[b],
                    ctx.R(ctx.insn.operands[1]), 1u << (base_bit + 3 - b));
      }
    }
  }
  return true;
}

bool BuildMtctr(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64;", ctx.CTR(), ctx.R(ctx.insn.operands[0]));
  return true;
}

bool BuildMtlr(BuilderContext& ctx) {
  if (!ctx.GetConfig().skip_lr)
    ctx.Println("\tctx.lr = {}.u64;", ctx.R(ctx.insn.operands[0]));
  return true;
}

bool BuildMtmsrd(BuilderContext& ctx) {
  if (!ctx.GetConfig().skip_msr) {
    ctx.Println("\tctx.msr = ({}.u32 & 0x8020) | (ctx.msr & ~0x8020);",
                ctx.R(ctx.insn.operands[0]));
    // r13 enters the global lock (interrupts off), any other source register leaves it
    u32 src_reg = ctx.insn.operands[0];
    if (src_reg == 13) {
      ctx.Println("\tREX_ENTER_GLOBAL_LOCK();");
    } else {
      ctx.Println("\tREX_LEAVE_GLOBAL_LOCK();");
    }
  }
  return true;
}

bool BuildMtfsf(BuilderContext& ctx) {
  u32 fm = ctx.insn.operands[0];
  u32 mask = 0;
  for (int j = 0; j < 8; j++) {
    if (fm & (1 << (7 - j)))
      mask |= 0xF << (4 * j);
  }
  if (mask == 0xFFFFFFFF) {
    ctx.Println("\tctx.fpscr.StoreFromGuest({}.u32);", ctx.F(ctx.insn.operands[1]));
  } else {
    ctx.Println(
        "\tctx.fpscr.StoreFromGuest((ctx.fpscr.LoadFromHost() & 0x{:08X}) | ({}.u32 & 0x{:08X}));",
        ~mask, ctx.F(ctx.insn.operands[1]), mask);
  }
  return true;
}

bool BuildMtxer(BuilderContext& ctx) {
  ctx.Println("\t{}.so = ({}.u64 & 0x80000000) != 0;", ctx.XER(), ctx.R(ctx.insn.operands[0]));
  ctx.Println("\t{}.ov = ({}.u64 & 0x40000000) != 0;", ctx.XER(), ctx.R(ctx.insn.operands[0]));
  ctx.Println("\t{}.ca = ({}.u64 & 0x20000000) != 0;", ctx.XER(), ctx.R(ctx.insn.operands[0]));
  return true;
}

bool BuildClrldi(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 & 0x{:X};", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), (1ull << (64 - ctx.insn.operands[2])) - 1);
  EmitRecordFormCompare(ctx);
  return true;
}

}  // namespace rex::codegen
