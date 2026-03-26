/**
 * @file        rexcodegen/builders/floating_point.cpp
 * @brief       PPC floating point instruction code generation
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

//=============================================================================
// Sign Manipulation
//=============================================================================

bool build_fabs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.u64 = {}.u64 & ~0x8000000000000000;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fnabs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.u64 = {}.u64 | 0x8000000000000000;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fneg(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.u64 = {}.u64 ^ 0x8000000000000000;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

//=============================================================================
// Move and Conversion
//=============================================================================

bool build_fmr(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64;", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fcfid(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double({}.s64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fctid(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println(
      "\t{0}.s64 = std::isnan({1}.f64) ? int64_t(0x8000000000000000ULL) : "
      "({1}.f64 > double(LLONG_MAX)) ? LLONG_MAX : "
      "simde_mm_cvtsd_si64(simde_mm_load_sd(&{1}.f64));",
      ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fctidz(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println(
      "\t{0}.s64 = std::isnan({1}.f64) ? int64_t(0x8000000000000000ULL) : "
      "({1}.f64 > double(LLONG_MAX)) ? LLONG_MAX : "
      "simde_mm_cvttsd_si64(simde_mm_load_sd(&{1}.f64));",
      ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fctiw(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println(
      "\t{0}.s64 = std::isnan({1}.f64) ? int64_t(0x80000000U) : "
      "({1}.f64 > double(INT_MAX)) ? INT_MAX : "
      "simde_mm_cvtsd_si32(simde_mm_load_sd(&{1}.f64));",
      ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fctiwz(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println(
      "\t{0}.s64 = std::isnan({1}.f64) ? int64_t(0x80000000U) : "
      "({1}.f64 > double(INT_MAX)) ? INT_MAX : "
      "simde_mm_cvttsd_si32(simde_mm_load_sd(&{1}.f64));",
      ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_frsp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float({}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

//=============================================================================
// Comparison
//=============================================================================

bool build_fcmpu(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.compare({}.f64, {}.f64);", ctx.cr(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fcmpo(BuilderContext& ctx) {
  // fcmpo is identical to fcmpu for recompilation purposes.
  // The difference is that fcmpo sets FPSCR exception flags for SNaN operands,
  // which we don't need to emulate.
  return build_fcmpu(ctx);
}

//=============================================================================
// Addition
//=============================================================================

bool build_fadd(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 + {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fadds(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float({}.f64 + {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

//=============================================================================
// Subtraction
//=============================================================================

bool build_fsub(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 - {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fsubs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float({}.f64 - {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

//=============================================================================
// Multiplication
//=============================================================================

bool build_fmul(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 * {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fmuls(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float({}.f64 * {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

//=============================================================================
// Division
//=============================================================================

bool build_fdiv(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 / {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fdivs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float({}.f64 / {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

//=============================================================================
// Fused Multiply-Add
//=============================================================================

bool build_fmadd(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 * {}.f64 + {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fmadds(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float({}.f64 * {}.f64 + {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fmsub(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 * {}.f64 - {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fmsubs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float({}.f64 * {}.f64 - {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fnmadd(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = -({}.f64 * {}.f64 + {}.f64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fnmadds(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float(-({}.f64 * {}.f64 + {}.f64)));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fnmsub(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = -({}.f64 * {}.f64 - {}.f64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fnmsubs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float(-({}.f64 * {}.f64 - {}.f64)));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

//=============================================================================
// Reciprocal and Square Root
//=============================================================================

bool build_fres(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float(1.0 / {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_frsqrte(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = 1.0 / sqrt({}.f64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fsqrt(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = sqrt({}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fsqrts(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float(sqrt({}.f64)));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

//=============================================================================
// Selection
//=============================================================================

bool build_fsel(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 >= 0.0 ? {}.f64 : {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

}  // namespace rex::codegen
