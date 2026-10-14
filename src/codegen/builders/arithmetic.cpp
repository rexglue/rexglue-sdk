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
#include "helpers.h"
namespace rex::codegen {

bool BuildAdd(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 + {}.u64;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildAdde(BuilderContext& ctx) {
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

bool BuildAddi(BuilderContext& ctx) {
  ctx.Print("\t{}.s64 = ", ctx.R(ctx.insn.operands[0]));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.s64 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{};", static_cast<i32>(ctx.insn.operands[2]));
  return true;
}

bool BuildAddic(BuilderContext& ctx) {
  ctx.Println("\t{}.ca = {}.u32 > {};", ctx.XER(), ctx.R(ctx.insn.operands[1]),
              ~ctx.insn.operands[2]);
  ctx.Println("\t{}.s64 = {}.s64 + {};", ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]),
              static_cast<i32>(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildAddis(BuilderContext& ctx) {
  ctx.Print("\t{}.s64 = ", ctx.R(ctx.insn.operands[0]));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.s64 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{};", static_cast<i32>(ctx.insn.operands[2] << 16));
  return true;
}

bool BuildAddze(BuilderContext& ctx) {
  ctx.Println("\t{}.s64 = {}.s64 + {}.ca;", ctx.Temp(), ctx.R(ctx.insn.operands[1]), ctx.XER());
  ctx.Println("\t{}.ca = {}.u32 < {}.u32;", ctx.XER(), ctx.Temp(), ctx.R(ctx.insn.operands[1]));
  ctx.Println("\t{}.s64 = {}.s64;", ctx.R(ctx.insn.operands[0]), ctx.Temp());
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildAddme(BuilderContext& ctx) {
  ctx.Println("\t{}.u8 = ({}.u32 + 0xFFFFFFFFu < {}.u32) | ({}.u32 + 0xFFFFFFFFu + {}.ca < {}.ca);",
              ctx.Temp(), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[1]), ctx.XER(), ctx.XER());
  ctx.Println("\t{}.u64 = {}.u64 + {}.ca + 0xFFFFFFFFFFFFFFFFull;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.XER());
  ctx.Println("\t{}.ca = {}.u8;", ctx.XER(), ctx.Temp());
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildAddc(BuilderContext& ctx) {
  ctx.Println("\t{}.ca = {}.u32 + {}.u32 < {}.u32;", ctx.XER(), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]), ctx.R(ctx.insn.operands[1]));
  ctx.Println("\t{}.u64 = {}.u64 + {}.u64;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

// PPC division does not trap, so a zero divisor or MIN / -1 (UB in C) yields 0.

bool BuildDivd(BuilderContext& ctx) {
  auto r_d = ctx.R(ctx.insn.operands[0]);
  auto r_a = ctx.R(ctx.insn.operands[1]);
  auto r_b = ctx.R(ctx.insn.operands[2]);
  ctx.Println(
      "\t{}.s64 = ({}.s64 && !({}.s64 == INT64_MIN && {}.s64 == -1)) ? {}.s64 / {}.s64 : 0;", r_d,
      r_b, r_a, r_b, r_a, r_b);
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildDivdu(BuilderContext& ctx) {
  auto r_d = ctx.R(ctx.insn.operands[0]);
  auto r_a = ctx.R(ctx.insn.operands[1]);
  auto r_b = ctx.R(ctx.insn.operands[2]);
  ctx.Println("\t{}.u64 = {}.u64 ? {}.u64 / {}.u64 : 0;", r_d, r_b, r_a, r_b);
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildDivw(BuilderContext& ctx) {
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

bool BuildDivwu(BuilderContext& ctx) {
  auto r_d = ctx.R(ctx.insn.operands[0]);
  auto r_a = ctx.R(ctx.insn.operands[1]);
  auto r_b = ctx.R(ctx.insn.operands[2]);
  ctx.Println("\t{}.u64 = u32({}.u32 ? {}.u32 / {}.u32 : 0);", r_d, r_b, r_a, r_b);
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildMulhw(BuilderContext& ctx) {
  ctx.Println("\t{}.s64 = (i64({}.s32) * i64({}.s32)) >> 32;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildMulhwu(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = (u64({}.u32) * u64({}.u32)) >> 32;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildMulld(BuilderContext& ctx) {
  // Use unsigned multiplication to avoid signed overflow UB (PPC wraps on overflow)
  ctx.Println("\t{}.s64 = static_cast<i64>({}.u64 * {}.u64);", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildMulli(BuilderContext& ctx) {
  // Use unsigned multiplication to avoid signed overflow UB (PPC wraps on overflow)
  ctx.Println("\t{}.s64 = static_cast<i64>({}.u64 * static_cast<u64>({}));",
              ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]),
              static_cast<i32>(ctx.insn.operands[2]));
  return true;
}

bool BuildMullw(BuilderContext& ctx) {
  ctx.Println("\t{}.s64 = i64({}.s32) * i64({}.s32);", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildMulhd(BuilderContext& ctx) {
  ctx.Println(
      "\t{}.s64 = static_cast<i64>((static_cast<__int128>(static_cast<i64>({}.s64)) * "
      "static_cast<__int128>(static_cast<i64>({}.s64))) >> 64);",
      ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildMulhdu(BuilderContext& ctx) {
  ctx.Println(
      "\t{}.u64 = static_cast<u64>((static_cast<__uint128_t>({}.u64) * "
      "static_cast<__uint128_t>({}.u64)) >> 64);",
      ctx.R(ctx.insn.operands[0]), ctx.R(ctx.insn.operands[1]), ctx.R(ctx.insn.operands[2]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildNeg(BuilderContext& ctx) {
  // Use unsigned negation to avoid signed overflow UB when negating INT64_MIN
  ctx.Println("\t{}.s64 = static_cast<i64>(-{}.u64);", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildSubf(BuilderContext& ctx) {
  ctx.Println("\t{}.u64 = {}.u64 - {}.u64;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[2]), ctx.R(ctx.insn.operands[1]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildSubfc(BuilderContext& ctx) {
  ctx.Println("\t{}.ca = {}.u32 >= {}.u32;", ctx.XER(), ctx.R(ctx.insn.operands[2]),
              ctx.R(ctx.insn.operands[1]));
  ctx.Println("\t{}.u64 = {}.u64 - {}.u64;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[2]), ctx.R(ctx.insn.operands[1]));
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildSubfe(BuilderContext& ctx) {
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

bool BuildSubfic(BuilderContext& ctx) {
  ctx.Println("\t{}.ca = {}.u32 <= {};", ctx.XER(), ctx.R(ctx.insn.operands[1]),
              ctx.insn.operands[2]);
  ctx.Println("\t{}.u64 = static_cast<u64>({}) - {}.u64;", ctx.R(ctx.insn.operands[0]),
              static_cast<i32>(ctx.insn.operands[2]), ctx.R(ctx.insn.operands[1]));
  return true;
}

bool BuildSubfze(BuilderContext& ctx) {
  ctx.Println("\t{}.u8 = ~{}.u32 + {}.ca < ~{}.u32;", ctx.Temp(), ctx.R(ctx.insn.operands[1]),
              ctx.XER(), ctx.R(ctx.insn.operands[1]));
  ctx.Println("\t{}.u64 = ~{}.u64 + {}.ca;", ctx.R(ctx.insn.operands[0]),
              ctx.R(ctx.insn.operands[1]), ctx.XER());
  ctx.Println("\t{}.ca = {}.u8;", ctx.XER(), ctx.Temp());
  EmitRecordFormCompare(ctx);
  return true;
}

bool BuildSubfme(BuilderContext& ctx) {
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
