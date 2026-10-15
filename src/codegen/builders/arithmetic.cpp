/**
 * @file        codegen/builders/arithmetic.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "builders.h"

#include <dis-asm.h>
#include <ppc-inst.h>

namespace rex::codegen {

template <>
bool Build<PPC_INST_ADD>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 + {}.u64;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_ADDE>(BuilderContext& ctx) {
  ctx.Println("\t{}.u8 = ({}.u32 + {}.u32 < {}.u32) | ({}.u32 + {}.u32 + {}.ca < {}.ca);",
              ctx.Temp(), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]),
              ctx.XER(), ctx.XER());
  ctx.Println("\t{}.u64 = {}.u64 + {}.u64 + {}.ca;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]), ctx.XER());
  ctx.Println("\t{}.ca = {}.u8;", ctx.XER(), ctx.Temp());
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_ADDI>(BuilderContext& ctx) {
  ctx.Print("\t{}.s64 = ", ctx.R(ctx.insn.operands[0]));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.s64 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{};", static_cast<i32>(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_ADDIC>(BuilderContext& ctx) {
  ctx.Println("\t{}.ca = {}.u32 > {};", ctx.XER(), ctx.R(ctx.insn.operands[1]),
              ~ctx.insn.operands[2]);
  ctx.Println("\t{}.s64 = {}.s64 + {};", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]),
              static_cast<i32>(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_ADDIS>(BuilderContext& ctx) {
  ctx.Print("\t{}.s64 = ", ctx.R(ctx.insn.operands[0]));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.s64 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{};", static_cast<i32>(ctx.insn.operands[2] << 16));
  return true;
}

template <>
bool Build<PPC_INST_ADDZE>(BuilderContext& ctx) {
  ctx.Println("\t{}.s64 = {}.s64 + {}.ca;", ctx.Temp(), ctx.R(ctx.insn.operands[1]), ctx.XER());
  ctx.Println("\t{}.ca = {}.u32 < {}.u32;", ctx.XER(), ctx.Temp(), ctx.R(ctx.insn.operands[1]));
  ctx.Println("\t{}.s64 = {}.s64;", ctx.R(ctx.insn.operands[0]), ctx.Temp());
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_ADDME>(BuilderContext& ctx) {
  ctx.Println("\t{}.u8 = ({}.u32 + 0xFFFFFFFFu < {}.u32) | ({}.u32 + 0xFFFFFFFFu + {}.ca < {}.ca);",
              ctx.Temp(), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[1]), ctx.XER(), ctx.XER());
  ctx.Println("\t{}.u64 = {}.u64 + {}.ca + 0xFFFFFFFFFFFFFFFFull;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.XER());
  ctx.Println("\t{}.ca = {}.u8;", ctx.XER(), ctx.Temp());
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_ADDC>(BuilderContext& ctx) {
  ctx.Println("\t{}.ca = {}.u32 + {}.u32 < {}.u32;", ctx.XER(), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]), ctx.R(ctx.insn.operands[1]));
  ctx.Println("\t{}.u64 = {}.u64 + {}.u64;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

// PPC division does not trap, so a zero divisor or MIN / -1 (UB in C) yields 0.

template <>
bool Build<PPC_INST_DIVD>(BuilderContext& ctx) {
  auto r_d = ctx.R(ctx.insn.operands[0]);
  auto r_a = ctx.R(ctx.insn.operands[1]);
  auto r_b = ctx.R(ctx.insn.operands[2]);
  ctx.Println(
      "\t{}.s64 = ({}.s64 && !({}.s64 == INT64_MIN && {}.s64 == -1)) ? {}.s64 / {}.s64 : 0;", r_d,
      r_b, r_a, r_b, r_a, r_b);
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_DIVDU>(BuilderContext& ctx) {
  auto r_d = ctx.R(ctx.insn.operands[0]);
  auto r_a = ctx.R(ctx.insn.operands[1]);
  auto r_b = ctx.R(ctx.insn.operands[2]);
  ctx.Println("\t{}.u64 = {}.u64 ? {}.u64 / {}.u64 : 0;", r_d, r_b, r_a, r_b);
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_DIVW>(BuilderContext& ctx) {
  auto r_d = ctx.R(ctx.insn.operands[0]);
  auto r_a = ctx.R(ctx.insn.operands[1]);
  auto r_b = ctx.R(ctx.insn.operands[2]);
  ctx.Println(
      "\t{}.u64 = u32(({}.s32 && !({}.s32 == INT32_MIN && {}.s32 == -1)) ? {}.s32 / {}.s32 : "
      "0);",
      r_d, r_b, r_a, r_b, r_a, r_b);
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_DIVWU>(BuilderContext& ctx) {
  auto r_d = ctx.R(ctx.insn.operands[0]);
  auto r_a = ctx.R(ctx.insn.operands[1]);
  auto r_b = ctx.R(ctx.insn.operands[2]);
  ctx.Println("\t{}.u64 = u32({}.u32 ? {}.u32 / {}.u32 : 0);", r_d, r_b, r_a, r_b);
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_MULHW>(BuilderContext& ctx) {
  ctx.Println("\t{}.s64 = (i64({}.s32) * i64({}.s32)) >> 32;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_MULHWU>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = (u64({}.u32) * u64({}.u32)) >> 32;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_MULLD>(BuilderContext& ctx) {
  // Use unsigned multiplication to avoid signed overflow UB (PPC wraps on overflow)
  ctx.Println("\t{}.s64 = static_cast<i64>({}.u64 * {}.u64);", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_MULLI>(BuilderContext& ctx) {
  // Use unsigned multiplication to avoid signed overflow UB (PPC wraps on overflow)
  ctx.Println("\t{}.s64 = static_cast<i64>({}.u64 * static_cast<u64>({}));",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]),
              static_cast<i32>(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_MULLW>(BuilderContext& ctx) {
  ctx.Println("\t{}.s64 = i64({}.s32) * i64({}.s32);", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_MULHD>(BuilderContext& ctx) {
  ctx.Println(
      "\t{}.s64 = static_cast<i64>((static_cast<__int128>(static_cast<i64>({}.s64)) * "
      "static_cast<__int128>(static_cast<i64>({}.s64))) >> 64);",
      ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_MULHDU>(BuilderContext& ctx) {
  ctx.Println(
      "\t{}.u64 = static_cast<u64>((static_cast<__uint128_t>({}.u64) * "
      "static_cast<__uint128_t>({}.u64)) >> 64);",
      ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_NEG>(BuilderContext& ctx) {
  // Use unsigned negation to avoid signed overflow UB when negating INT64_MIN
  ctx.Println("\t{}.s64 = static_cast<i64>(-{}.u64);", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_SUBF>(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 - {}.u64;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[2]), ctx.R(ctx.insn.operands[1]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_SUBFC>(BuilderContext& ctx) {
  ctx.Println("\t{}.ca = {}.u32 >= {}.u32;", ctx.XER(), ctx.R(ctx.insn.operands[2]),
              ctx.R(ctx.insn.operands[1]));
  ctx.Println("\t{}.u64 = {}.u64 - {}.u64;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[2]), ctx.R(ctx.insn.operands[1]));
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_SUBFE>(BuilderContext& ctx) {
  ctx.Println("\t{}.u8 = (~{}.u32 + {}.u32 < ~{}.u32) | (~{}.u32 + {}.u32 + {}.ca < {}.ca);",
              ctx.Temp(), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]),
              ctx.XER(), ctx.XER());
  ctx.Println("\t{}.u64 = ~{}.u64 + {}.u64 + {}.ca;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]), ctx.XER());
  ctx.Println("\t{}.ca = {}.u8;", ctx.XER(), ctx.Temp());
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_SUBFIC>(BuilderContext& ctx) {
  ctx.Println("\t{}.ca = {}.u32 <= {};", ctx.XER(), ctx.R(ctx.insn.operands[1]),
              ctx.insn.operands[2]);
  ctx.Println("\t{}.u64 = static_cast<u64>({}) - {}.u64;", ctx.R(ctx.insn.operands[0]),
              static_cast<i32>(ctx.insn.operands[2]), ctx.R(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_SUBFZE>(BuilderContext& ctx) {
  ctx.Println("\t{}.u8 = ~{}.u32 + {}.ca < ~{}.u32;", ctx.Temp(), ctx.R(ctx.insn.operands[1]),
              ctx.XER(), ctx.R(ctx.insn.operands[1]));
  ctx.Println("\t{}.u64 = ~{}.u64 + {}.ca;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.XER());
  ctx.Println("\t{}.ca = {}.u8;", ctx.XER(), ctx.Temp());
  EmitRecordFormCompare(ctx);
  return true;
}

template <>
bool Build<PPC_INST_SUBFME>(BuilderContext& ctx) {
  ctx.Println(
      "\t{}.u8 = (~{}.u32 + 0xFFFFFFFFu < ~{}.u32) | (~{}.u32 + 0xFFFFFFFFu + {}.ca < {}.ca);",
      ctx.Temp(), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[1]),
      ctx.R(ctx.insn.operands[1]), ctx.XER(), ctx.XER());
  ctx.Println("\t{}.u64 = ~{}.u64 + {}.ca + 0xFFFFFFFFFFFFFFFFull;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.XER());
  ctx.Println("\t{}.ca = {}.u8;", ctx.XER(), ctx.Temp());
  EmitRecordFormCompare(ctx);
  return true;
}

}  // namespace rex::codegen
