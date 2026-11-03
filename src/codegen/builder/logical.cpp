/**
 * @file        codegen/builder/logical.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "builder.h"

#include <format>
#include <string_view>

#include <dis-asm.h>
#include <ppc-inst.h>

namespace rex::codegen {

namespace {

/**
 * Returns the 64 bit mask of the rotate and mask instructions, with bits mstart through mstop set.
 * The mask wraps when mstart is above mstop.
 *
 * @param mstart First bit, 0 to 63
 * @param mstop Last bit, 0 to 63
 */
u64 ComputeMask(u32 mstart, u32 mstop) {
  mstart &= 0x3F;
  mstop &= 0x3F;
  u64 value = (UINT64_MAX >> mstart) ^ ((mstop >= 63) ? 0 : UINT64_MAX >> (mstop + 1));
  return mstart <= mstop ? value : ~value;
}

/**
 * Emits the CR bit operation `crD = crA <op> crB` on bit indexes 0 to 31.
 *
 * @param op The operator, such as "|", "&" or "^"
 * @param invert_result True to invert the result before storing it in crD
 */
void EmitCRBitOperation(BuilderContext& ctx, std::string_view op, bool invert_a = false,
                        bool invert_b = false, bool invert_result = false) {
  u32 cr_d = ctx.insn.operands[0];
  u32 cr_a = ctx.insn.operands[1];
  u32 cr_b = ctx.insn.operands[2];

  u32 cr_field_d = cr_d / 4;
  u32 cr_bit_d = cr_d % 4;
  u32 cr_field_a = cr_a / 4;
  u32 cr_bit_a = cr_a % 4;
  u32 cr_field_b = cr_b / 4;
  u32 cr_bit_b = cr_b % 4;

  std::string a_expr = std::format("{}.{}", ctx.CR(cr_field_a), GetCRBitName(cr_bit_a));

  std::string b_expr = std::format("{}.{}", ctx.CR(cr_field_b), GetCRBitName(cr_bit_b));

  if (invert_a)
    a_expr = "!(" + a_expr + ")";
  if (invert_b)
    b_expr = "!(" + b_expr + ")";

  std::string expr = std::format("{} {} {}", a_expr, op, b_expr);

  if (invert_result)
    expr = "!(" + expr + ")";

  ctx.Println("\t{}.{} = {};", ctx.CR(cr_field_d), GetCRBitName(cr_bit_d), expr);
}

}  // namespace

template <>
bool Build<PPC_INST_AND>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 & {}.u64;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_ANDC>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 & ~{}.u64;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_ANDI>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 & {};", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]),
              ctx.insn.operands[2]);
  ctx.Println("\t{}.Compare<i32>({}.s32, 0, {});", ctx.CR(0), ctx.R(ctx.insn.operands[0]),
              ctx.XER());
  return true;
}

template <>
bool Build<PPC_INST_ANDIS>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 & {};", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]),
              ctx.insn.operands[2] << 16);
  ctx.Println("\t{}.Compare<i32>({}.s32, 0, {});", ctx.CR(0), ctx.R(ctx.insn.operands[0]),
              ctx.XER());
  return true;
}

template <>
bool Build<PPC_INST_NAND>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = ~({}.u64 & {}.u64);", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_NOR>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = ~({}.u64 | {}.u64);", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_NOT>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = ~{}.u64;", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_OR>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 | {}.u64;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_ORC>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 | ~{}.u64;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_ORI>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 | {};", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]),
              ctx.insn.operands[2]);
  return true;
}

template <>
bool Build<PPC_INST_ORIS>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 | {};", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]),
              static_cast<u32>(ctx.insn.operands[2]) << 16);
  return true;
}

template <>
bool Build<PPC_INST_XOR>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 ^ {}.u64;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_XORI>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 ^ {};", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]),
              ctx.insn.operands[2]);
  return true;
}

template <>
bool Build<PPC_INST_XORIS>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 ^ {};", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]),
              ctx.insn.operands[2] << 16);
  return true;
}

template <>
bool Build<PPC_INST_EQV>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = ~({}.u64 ^ {}.u64);", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_CNTLZD>(BuilderContext& ctx) {
  ctx.Println("\t{0}.u64 = {1}.u64 == 0 ? 64 : __builtin_clzll({1}.u64);",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_CNTLZW>(BuilderContext& ctx) {
  ctx.Println("\t{0}.u64 = {1}.u32 == 0 ? 32 : __builtin_clz({1}.u32);",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_EXTSB>(BuilderContext& ctx) {
  ctx.Println("\t{}.s64 = {}.s8;", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_EXTSH>(BuilderContext& ctx) {
  ctx.Println("\t{}.s64 = {}.s16;", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_EXTSW>(BuilderContext& ctx) {
  ctx.Println("\t{}.s64 = {}.s32;", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_CLRLWI>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u32 & 0x{:X};", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), (1ull << (32 - ctx.insn.operands[2])) - 1);
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_RLDICL>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = __builtin_rotateleft64({}.u64, {}) & 0x{:X};",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.insn.operands[2],
              ComputeMask(ctx.insn.operands[3], 63));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_RLDICR>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = __builtin_rotateleft64({}.u64, {}) & 0x{:X};",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.insn.operands[2],
              ComputeMask(0, ctx.insn.operands[3]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_RLDIC>(BuilderContext& ctx) {
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

template <>
bool Build<PPC_INST_RLDCL>(BuilderContext& ctx) {
  u64 mask = ComputeMask(ctx.insn.operands[3], 63);
  ctx.Println("\t{}.u64 = __builtin_rotateleft64({}.u64, {}.u8 & 0x3F) & 0x{:X};",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]),
              mask);
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_RLDCR>(BuilderContext& ctx) {
  u64 mask = ComputeMask(0, ctx.insn.operands[3]);
  ctx.Println("\t{}.u64 = __builtin_rotateleft64({}.u64, {}.u8 & 0x3F) & 0x{:X};",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]),
              mask);
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_RLDIMI>(BuilderContext& ctx) {
  const u64 mask = ComputeMask(ctx.insn.operands[3], ~ctx.insn.operands[2]);
  ctx.Println("\t{}.u64 = (__builtin_rotateleft64({}.u64, {}) & 0x{:X}) | ({}.u64 & 0x{:X});",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.insn.operands[2], mask,
              ctx.R(ctx.insn.operands[0]), ~mask);
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_ROTLDI>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = __builtin_rotateleft64({}.u64, {});", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.insn.operands[2]);
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_RLWIMI>(BuilderContext& ctx) {
  const u64 mask = ComputeMask(ctx.insn.operands[3] + 32, ctx.insn.operands[4] + 32);
  ctx.Println(
      "\t{}.u64 = (__builtin_rotateleft64({}.u32 | ({}.u64 << 32), {}) & 0x{:X}) | ({}.u64 & "
      "0x{:X});",
      ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[1]),
      ctx.insn.operands[2], mask, ctx.R(ctx.insn.operands[0]), ~mask);
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_RLWINM>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = __builtin_rotateleft64({}.u32 | ({}.u64 << 32), {}) & 0x{:X};",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[1]),
              ctx.insn.operands[2],
              ComputeMask(ctx.insn.operands[3] + 32, ctx.insn.operands[4] + 32));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_RLWNM>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = __builtin_rotateleft64({}.u32 | ({}.u64 << 32), {}.u8 & 0x1F) & 0x{:X};",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]),
              ComputeMask(ctx.insn.operands[3] + 32, ctx.insn.operands[4] + 32));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_ROTLW>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = __builtin_rotateleft32({}.u32, {}.u8 & 0x1F);",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_ROTLWI>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = __builtin_rotateleft32({}.u32, {});", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.insn.operands[2]);
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_SLD>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u8 & 0x40 ? 0 : ({}.u64 << ({}.u8 & 0x7F));",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[2]), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_SLW>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u8 & 0x20 ? 0 : ({}.u32 << ({}.u8 & 0x3F));",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[2]), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_SRAD>(BuilderContext& ctx) {
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

template <>
bool Build<PPC_INST_SRADI>(BuilderContext& ctx) {
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

template <>
bool Build<PPC_INST_SRAW>(BuilderContext& ctx) {
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

template <>
bool Build<PPC_INST_SRAWI>(BuilderContext& ctx) {
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

template <>
bool Build<PPC_INST_SRD>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u8 & 0x40 ? 0 : ({}.u64 >> ({}.u8 & 0x7F));",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[2]), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_SRW>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u8 & 0x20 ? 0 : ({}.u32 >> ({}.u8 & 0x3F));",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[2]), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_CRAND>(BuilderContext& ctx) {
  EmitCRBitOperation(ctx, "&");
  return true;
}

template <>
bool Build<PPC_INST_CRANDC>(BuilderContext& ctx) {
  EmitCRBitOperation(ctx, "&", false, true, false);
  return true;
}

template <>
bool Build<PPC_INST_CREQV>(BuilderContext& ctx) {
  EmitCRBitOperation(ctx, "^", false, false, true);
  return true;
}

template <>
bool Build<PPC_INST_CRNAND>(BuilderContext& ctx) {
  EmitCRBitOperation(ctx, "&", false, false, true);
  return true;
}

template <>
bool Build<PPC_INST_CRNOR>(BuilderContext& ctx) {
  EmitCRBitOperation(ctx, "|", false, false, true);
  return true;
}

template <>
bool Build<PPC_INST_CROR>(BuilderContext& ctx) {
  EmitCRBitOperation(ctx, "|");
  return true;
}

template <>
bool Build<PPC_INST_CRORC>(BuilderContext& ctx) {
  EmitCRBitOperation(ctx, "|", false, true, false);
  return true;
}

template <>
bool Build<PPC_INST_CRXOR>(BuilderContext& ctx) {
  EmitCRBitOperation(ctx, "^");
  return true;
}

}  // namespace rex::codegen
