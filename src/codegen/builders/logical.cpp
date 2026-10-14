/**
 * @file        codegen/builders/logical.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "helpers.h"

namespace rex::codegen {

bool BuildAnd(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 & {}.u64;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildAndc(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 & ~{}.u64;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildAndi(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 & {};", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]),
              ctx.insn.operands[2]);
  ctx.Println("\t{}.Compare<i32>({}.s32, 0, {});", ctx.CR(0), ctx.R(ctx.insn.operands[0]),
              ctx.XER());
  return true;
}

bool BuildAndis(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 & {};", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]),
              ctx.insn.operands[2] << 16);
  ctx.Println("\t{}.Compare<i32>({}.s32, 0, {});", ctx.CR(0), ctx.R(ctx.insn.operands[0]),
              ctx.XER());
  return true;
}

bool BuildNand(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = ~({}.u64 & {}.u64);", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildNor(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = ~({}.u64 | {}.u64);", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildNot(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = ~{}.u64;", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildOr(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 | {}.u64;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildOrc(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 | ~{}.u64;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildOri(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 | {};", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]),
              ctx.insn.operands[2]);
  return true;
}

bool BuildOris(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 | {};", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]),
              static_cast<u32>(ctx.insn.operands[2]) << 16);
  return true;
}

bool BuildXor(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 ^ {}.u64;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildXori(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 ^ {};", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]),
              ctx.insn.operands[2]);
  return true;
}

bool BuildXoris(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 ^ {};", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]),
              ctx.insn.operands[2] << 16);
  return true;
}

bool BuildEqv(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = ~({}.u64 ^ {}.u64);", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildCntlzd(BuilderContext& ctx) {
  ctx.Println("\t{0}.u64 = {1}.u64 == 0 ? 64 : __builtin_clzll({1}.u64);",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildCntlzw(BuilderContext& ctx) {
  ctx.Println("\t{0}.u64 = {1}.u32 == 0 ? 32 : __builtin_clz({1}.u32);",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildExtsb(BuilderContext& ctx) {
  ctx.Println("\t{}.s64 = {}.s8;", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildExtsh(BuilderContext& ctx) {
  ctx.Println("\t{}.s64 = {}.s16;", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildExtsw(BuilderContext& ctx) {
  ctx.Println("\t{}.s64 = {}.s32;", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildClrlwi(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u32 & 0x{:X};", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), (1ull << (32 - ctx.insn.operands[2])) - 1);
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildRldicl(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = __builtin_rotateleft64({}.u64, {}) & 0x{:X};",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.insn.operands[2],
              ComputeMask(ctx.insn.operands[3], 63));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildRldicr(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = __builtin_rotateleft64({}.u64, {}) & 0x{:X};",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.insn.operands[2],
              ComputeMask(0, ctx.insn.operands[3]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildRldic(BuilderContext& ctx) {
  u32 sh = ctx.insn.operands[2];
  u64 mask = ComputeMask(ctx.insn.operands[3], 63 - sh);
  if (sh)
    ctx.Println("\t{}.u64 = __builtin_rotateleft64({}.u64, {}) & 0x{:X};",
                ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), sh, mask);
  else
    ctx.Println("\t{}.u64 = {}.u64 & 0x{:X};", ctx.R(ctx.insn.operands[0]),
                ctx.R(ctx.insn.operands[1]), mask);
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildRldcl(BuilderContext& ctx) {
  u64 mask = ComputeMask(ctx.insn.operands[3], 63);
  ctx.Println("\t{}.u64 = __builtin_rotateleft64({}.u64, {}.u8 & 0x3F) & 0x{:X};",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]),
              mask);
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildRldcr(BuilderContext& ctx) {
  u64 mask = ComputeMask(0, ctx.insn.operands[3]);
  ctx.Println("\t{}.u64 = __builtin_rotateleft64({}.u64, {}.u8 & 0x3F) & 0x{:X};",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]),
              mask);
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildRldimi(BuilderContext& ctx) {
  const u64 mask = ComputeMask(ctx.insn.operands[3], ~ctx.insn.operands[2]);
  ctx.Println("\t{}.u64 = (__builtin_rotateleft64({}.u64, {}) & 0x{:X}) | ({}.u64 & 0x{:X});",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.insn.operands[2], mask,
              ctx.R(ctx.insn.operands[0]), ~mask);
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildRotldi(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = __builtin_rotateleft64({}.u64, {});", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.insn.operands[2]);
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildRlwimi(BuilderContext& ctx) {
  const u64 mask = ComputeMask(ctx.insn.operands[3] + 32, ctx.insn.operands[4] + 32);
  ctx.Println(
      "\t{}.u64 = (__builtin_rotateleft64({}.u32 | ({}.u64 << 32), {}) & 0x{:X}) | ({}.u64 & "
      "0x{:X});",
      ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[1]),
      ctx.insn.operands[2], mask, ctx.R(ctx.insn.operands[0]), ~mask);
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildRlwinm(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = __builtin_rotateleft64({}.u32 | ({}.u64 << 32), {}) & 0x{:X};",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[1]),
              ctx.insn.operands[2],
              ComputeMask(ctx.insn.operands[3] + 32, ctx.insn.operands[4] + 32));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildRlwnm(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = __builtin_rotateleft64({}.u32 | ({}.u64 << 32), {}.u8 & 0x1F) & 0x{:X};",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]),
              ComputeMask(ctx.insn.operands[3] + 32, ctx.insn.operands[4] + 32));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildRotlw(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = __builtin_rotateleft32({}.u32, {}.u8 & 0x1F);",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildRotlwi(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = __builtin_rotateleft32({}.u32, {});", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.insn.operands[2]);
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildSld(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u8 & 0x40 ? 0 : ({}.u64 << ({}.u8 & 0x7F));",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[2]), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildSlw(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u8 & 0x20 ? 0 : ({}.u32 << ({}.u8 & 0x3F));",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[2]), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildSrad(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 & 0x7F;", ctx.Temp(), ctx.R(ctx.insn.operands[2]));
  ctx.Println("\tif ({}.u64 > 0x3F) {}.u64 = 0x3F;", ctx.Temp(), ctx.Temp());
  ctx.Println("\t{}.ca = ({}.s64 < 0) & ((({}.s64 >> {}.u64) << {}.u64) != {}.s64);", ctx.XER(),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[1]), ctx.Temp(), ctx.Temp(),
              ctx.R(ctx.insn.operands[1]));
  ctx.Println("\t{}.s64 = {}.s64 >> {}.u64;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.Temp());
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildSradi(BuilderContext& ctx) {
  if (ctx.insn.operands[2] != 0) {
    ctx.Println("\t{}.ca = ({}.s64 < 0) & (({}.u64 & 0x{:X}) != 0);", ctx.XER(),
                ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[1]),
                ComputeMask(64 - ctx.insn.operands[2], 63));
    ctx.Println("\t{}.s64 = {}.s64 >> {};", ctx.R(ctx.insn.operands[0]),
                ctx.R(ctx.insn.operands[1]), ctx.insn.operands[2]);
  } else {
    ctx.Println("\t{}.ca = 0;", ctx.XER());
    ctx.Println("\t{}.s64 = {}.s64;", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]));
  }
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildSraw(BuilderContext& ctx) {
  ctx.Println("\t{}.u32 = {}.u32 & 0x3F;", ctx.Temp(), ctx.R(ctx.insn.operands[2]));
  ctx.Println("\tif ({}.u32 > 0x1F) {}.u32 = 0x1F;", ctx.Temp(), ctx.Temp());
  ctx.Println("\t{}.ca = ({}.s32 < 0) & ((({}.s32 >> {}.u32) << {}.u32) != {}.s32);", ctx.XER(),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[1]), ctx.Temp(), ctx.Temp(),
              ctx.R(ctx.insn.operands[1]));
  ctx.Println("\t{}.s64 = {}.s32 >> {}.u32;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.Temp());
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildSrawi(BuilderContext& ctx) {
  if (ctx.insn.operands[2] != 0) {
    ctx.Println("\t{}.ca = ({}.s32 < 0) & (({}.u32 & 0x{:X}) != 0);", ctx.XER(),
                ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[1]),
                ComputeMask(64 - ctx.insn.operands[2], 63));
    ctx.Println("\t{}.s64 = {}.s32 >> {};", ctx.R(ctx.insn.operands[0]),
                ctx.R(ctx.insn.operands[1]), ctx.insn.operands[2]);
  } else {
    ctx.Println("\t{}.ca = 0;", ctx.XER());
    ctx.Println("\t{}.s64 = {}.s32;", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]));
  }
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildSrd(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u8 & 0x40 ? 0 : ({}.u64 >> ({}.u8 & 0x7F));",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[2]), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildSrw(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u8 & 0x20 ? 0 : ({}.u32 >> ({}.u8 & 0x3F));",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[2]), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildCrand(BuilderContext& ctx) {
  EmitCRBitOperation(ctx, "&");
  return true;
}

bool BuildCrandc(BuilderContext& ctx) {
  EmitCRBitOperation(ctx, "&", false, true, false);
  return true;
}

bool BuildCreqv(BuilderContext& ctx) {
  EmitCRBitOperation(ctx, "^", false, false, true);
  return true;
}

bool BuildCrnand(BuilderContext& ctx) {
  EmitCRBitOperation(ctx, "&", false, false, true);
  return true;
}

bool BuildCrnor(BuilderContext& ctx) {
  EmitCRBitOperation(ctx, "|", false, false, true);
  return true;
}

bool BuildCror(BuilderContext& ctx) {
  EmitCRBitOperation(ctx, "|");
  return true;
}

bool BuildCrorc(BuilderContext& ctx) {
  EmitCRBitOperation(ctx, "|", false, true, false);
  return true;
}

bool BuildCrxor(BuilderContext& ctx) {
  EmitCRBitOperation(ctx, "^");
  return true;
}

}  // namespace rex::codegen
