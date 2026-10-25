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

#include <dis-asm.h>
#include <ppc-inst.h>

namespace rex::codegen {

template <>
bool Build<PPC_INST_FABS>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.u64 = {}.u64 & ~0x8000000000000000;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FNABS>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.u64 = {}.u64 | 0x8000000000000000;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FNEG>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.u64 = {}.u64 ^ 0x8000000000000000;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FMR>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = {}.f64;", ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FCFID>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64({}.s64);", ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FCTID>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println(
      "\t{0}.s64 = std::isnan({1}.f64) ? i64(0x8000000000000000ULL) : "
      "({1}.f64 > f64(LLONG_MAX)) ? LLONG_MAX : "
      "simde_mm_cvtsd_si64(simde_mm_load_sd(&{1}.f64));",
      ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FCTIDZ>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println(
      "\t{0}.s64 = std::isnan({1}.f64) ? i64(0x8000000000000000ULL) : "
      "({1}.f64 > f64(LLONG_MAX)) ? LLONG_MAX : "
      "simde_mm_cvttsd_si64(simde_mm_load_sd(&{1}.f64));",
      ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FCTIW>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println(
      "\t{0}.s64 = std::isnan({1}.f64) ? i64(INT_MIN) : "
      "({1}.f64 >= f64(INT_MAX)) ? INT_MAX : "
      "simde_mm_cvtsd_si32(simde_mm_load_sd(&{1}.f64));",
      ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FCTIWZ>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println(
      "\t{0}.s64 = std::isnan({1}.f64) ? i64(INT_MIN) : "
      "({1}.f64 >= f64(INT_MAX)) ? INT_MAX : "
      "simde_mm_cvttsd_si32(simde_mm_load_sd(&{1}.f64));",
      ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FRSP>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32({}.f64));", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FCMPU>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.Compare({}.f64, {}.f64);", ctx.CR(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_FCMPO>(BuilderContext& ctx) {
  // fcmpo matches fcmpu here, its FPSCR exception flags for SNaN operands are not emulated.
  return Build<PPC_INST_FCMPU>(ctx);
}

template <>
bool Build<PPC_INST_FADD>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = {}.f64 + {}.f64;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_FADDS>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32({}.f64 + {}.f64));", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_FSUB>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = {}.f64 - {}.f64;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_FSUBS>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32({}.f64 - {}.f64));", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_FMUL>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = {}.f64 * {}.f64;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_FMULS>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32({}.f64 * {}.f64));", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_FDIV>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = {}.f64 / {}.f64;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_FDIVS>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32({}.f64 / {}.f64));", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_FMADD>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = std::fma({}.f64, {}.f64, {}.f64);", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]),
              ctx.F(ctx.insn.operands[3]));
  return true;
}

template <>
bool Build<PPC_INST_FMADDS>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32(std::fma({}.f64, {}.f64, {}.f64)));", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]),
              ctx.F(ctx.insn.operands[3]));
  return true;
}

template <>
bool Build<PPC_INST_FMSUB>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = std::fma({}.f64, {}.f64, -{}.f64);", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]),
              ctx.F(ctx.insn.operands[3]));
  return true;
}

template <>
bool Build<PPC_INST_FMSUBS>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32(std::fma({}.f64, {}.f64, -{}.f64)));",
              ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]),
              ctx.F(ctx.insn.operands[3]));
  return true;
}

template <>
bool Build<PPC_INST_FNMADD>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = -std::fma({}.f64, {}.f64, {}.f64);", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]),
              ctx.F(ctx.insn.operands[3]));
  return true;
}

template <>
bool Build<PPC_INST_FNMADDS>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32(-std::fma({}.f64, {}.f64, {}.f64)));",
              ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]),
              ctx.F(ctx.insn.operands[3]));
  return true;
}

template <>
bool Build<PPC_INST_FNMSUB>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = -std::fma({}.f64, {}.f64, -{}.f64);", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]),
              ctx.F(ctx.insn.operands[3]));
  return true;
}

template <>
bool Build<PPC_INST_FNMSUBS>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32(-std::fma({}.f64, {}.f64, -{}.f64)));",
              ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]),
              ctx.F(ctx.insn.operands[3]));
  return true;
}

template <>
bool Build<PPC_INST_FRES>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  if (ctx.GetConfig().vector_math.fres == VectorMath::kFast) {
    ctx.Println("	{}.f64 = f64(f32(1.0 / {}.f64));", ctx.F(ctx.insn.operands[0]),
                ctx.F(ctx.insn.operands[1]));
    return true;
  }
  ctx.Println("\t{}.f64 = rex::ppc::ReciprocalEstimate({}.f64);", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FRSQRTE>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  if (ctx.GetConfig().vector_math.frsqrte == VectorMath::kFast) {
    ctx.Println("	{}.f64 = f64(f32(1.0 / std::sqrt({}.f64)));", ctx.F(ctx.insn.operands[0]),
                ctx.F(ctx.insn.operands[1]));
    return true;
  }
  ctx.Println("\t{}.f64 = rex::ppc::ReciprocalSqrtEstimate({}.f64);", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FSQRT>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = sqrt({}.f64);", ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FSQRTS>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32(sqrt({}.f64)));", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FSEL>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = {}.f64 >= 0.0 ? {}.f64 : {}.f64;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]),
              ctx.F(ctx.insn.operands[3]));
  return true;
}

}  // namespace rex::codegen
