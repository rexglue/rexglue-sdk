/**
 * @file        codegen/builders/floating_point.cpp
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

bool BuildFabs(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.u64 = {}.u64 & ~0x8000000000000000;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

bool BuildFnabs(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.u64 = {}.u64 | 0x8000000000000000;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

bool BuildFneg(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.u64 = {}.u64 ^ 0x8000000000000000;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

bool BuildFmr(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = {}.f64;", ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

bool BuildFcfid(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64({}.s64);", ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

bool BuildFctid(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println(
      "\t{0}.s64 = std::isnan({1}.f64) ? i64(0x8000000000000000ULL) : "
      "({1}.f64 > f64(LLONG_MAX)) ? LLONG_MAX : "
      "simde_mm_cvtsd_si64(simde_mm_load_sd(&{1}.f64));",
      ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

bool BuildFctidz(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println(
      "\t{0}.s64 = std::isnan({1}.f64) ? i64(0x8000000000000000ULL) : "
      "({1}.f64 > f64(LLONG_MAX)) ? LLONG_MAX : "
      "simde_mm_cvttsd_si64(simde_mm_load_sd(&{1}.f64));",
      ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

bool BuildFctiw(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println(
      "\t{0}.s64 = std::isnan({1}.f64) ? i64(0x80000000U) : "
      "({1}.f64 >= f64(INT_MAX)) ? INT_MAX : "
      "simde_mm_cvtsd_si32(simde_mm_load_sd(&{1}.f64));",
      ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

bool BuildFctiwz(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println(
      "\t{0}.s64 = std::isnan({1}.f64) ? i64(0x80000000U) : "
      "({1}.f64 >= f64(INT_MAX)) ? INT_MAX : "
      "simde_mm_cvttsd_si32(simde_mm_load_sd(&{1}.f64));",
      ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

bool BuildFrsp(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32({}.f64));", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

bool BuildFcmpu(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.Compare({}.f64, {}.f64);", ctx.CR(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]));
  return true;
}

bool BuildFcmpo(BuilderContext& ctx) {
  // fcmpo matches fcmpu here, its FPSCR exception flags for SNaN operands are not emulated.
  return BuildFcmpu(ctx);
}

bool BuildFadd(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = {}.f64 + {}.f64;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]));
  return true;
}

bool BuildFadds(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32({}.f64 + {}.f64));", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]));
  return true;
}

bool BuildFsub(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = {}.f64 - {}.f64;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]));
  return true;
}

bool BuildFsubs(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32({}.f64 - {}.f64));", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]));
  return true;
}

bool BuildFmul(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = {}.f64 * {}.f64;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]));
  return true;
}

bool BuildFmuls(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32({}.f64 * {}.f64));", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]));
  return true;
}

bool BuildFdiv(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = {}.f64 / {}.f64;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]));
  return true;
}

bool BuildFdivs(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32({}.f64 / {}.f64));", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]));
  return true;
}

bool BuildFmadd(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = std::fma({}.f64, {}.f64, {}.f64);", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]),
              ctx.F(ctx.insn.operands[3]));
  return true;
}

bool BuildFmadds(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32(std::fma({}.f64, {}.f64, {}.f64)));", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]),
              ctx.F(ctx.insn.operands[3]));
  return true;
}

bool BuildFmsub(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = std::fma({}.f64, {}.f64, -{}.f64);", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]),
              ctx.F(ctx.insn.operands[3]));
  return true;
}

bool BuildFmsubs(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32(std::fma({}.f64, {}.f64, -{}.f64)));",
              ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]),
              ctx.F(ctx.insn.operands[3]));
  return true;
}

bool BuildFnmadd(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = -std::fma({}.f64, {}.f64, {}.f64);", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]),
              ctx.F(ctx.insn.operands[3]));
  return true;
}

bool BuildFnmadds(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32(-std::fma({}.f64, {}.f64, {}.f64)));",
              ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]),
              ctx.F(ctx.insn.operands[3]));
  return true;
}

bool BuildFnmsub(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = -std::fma({}.f64, {}.f64, -{}.f64);", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]),
              ctx.F(ctx.insn.operands[3]));
  return true;
}

bool BuildFnmsubs(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32(-std::fma({}.f64, {}.f64, -{}.f64)));",
              ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]),
              ctx.F(ctx.insn.operands[3]));
  return true;
}

bool BuildFres(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = rex::ppc::Fres({}.f64);", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

bool BuildFrsqrte(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = rex::ppc::Frsqrte({}.f64);", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

bool BuildFsqrt(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = sqrt({}.f64);", ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

bool BuildFsqrts(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32(sqrt({}.f64)));", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

bool BuildFsel(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = {}.f64 >= 0.0 ? {}.f64 : {}.f64;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]),
              ctx.F(ctx.insn.operands[3]));
  return true;
}

}  // namespace rex::codegen
