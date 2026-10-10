/**
 * @file        codegen/builders/vector.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "helpers.h"

#include <bit>
#include <cmath>

#include <simde/x86/sse.h>

#include <rex/logging.h>

#include <ppc.h>

namespace rex::codegen {

bool build_vaddfp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_binary("add");
  return true;
}

bool build_vsubfp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_binary("sub");
  return true;
}

bool build_vmulfp128(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_binary("mul");
  return true;
}

bool build_vmaddfp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_add_ps(simde_mm_mul_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32)), simde_mm_load_ps({}.f32)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]),
      ctx.v(ctx.insn.operands[3]));
  return true;
}

/**
 * Builds vnmsubfp, vD = -(vA * vB - vC). The negation is an XOR with the sign bit 0x80000000.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool build_vnmsubfp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.println(
      "\tsimde_mm_store_ps({}.f32, "
      "simde_mm_xor_ps(simde_mm_sub_ps(simde_mm_mul_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32)), simde_mm_load_ps({}.f32)), "
      "simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000)))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]),
      ctx.v(ctx.insn.operands[3]));
  return true;
}

bool build_vmaxfp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_binary("max");
  return true;
}

bool build_vminfp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_binary("min");
  return true;
}

bool build_vrefp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_unary_expr("simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_load_ps({vA}.f32))");
  return true;
}

bool build_vrsqrtefp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_unary_expr(
      "simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_sqrt_ps(simde_mm_load_ps({vA}.f32)))");
  return true;
}

/**
 * Builds vexptefp as an exp2 estimate with about 12 bit precision.
 *
 * exp2(x) is 2^n * poly(f) with n = floor(x) and f = x - n. The polynomial is a 4th order minimax
 * fit of 2^f on [0,1). 2^n is built by adding n to the exponent bias 127 and shifting into bit 23.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool build_vexptefp(BuilderContext& ctx) {
  auto vD = ctx.v(ctx.insn.operands[0]);
  auto vA = ctx.v(ctx.insn.operands[1]);
  ctx.emit_set_flush_mode(true);
  ctx.println("\t{{");
  ctx.println("\t\tsimde__m128 x = simde_mm_load_ps({}.f32);", vA);
  ctx.println(
      "\t\tsimde__m128 n = simde_mm_round_ps(x, "
      "SIMDE_MM_FROUND_TO_NEG_INF | SIMDE_MM_FROUND_NO_EXC);");
  ctx.println("\t\tsimde__m128 f = simde_mm_sub_ps(x, n);");
  ctx.println("\t\tsimde__m128 p = simde_mm_set1_ps(1.8775767e-3f);");
  ctx.println("\t\tp = simde_mm_add_ps(simde_mm_mul_ps(p, f), simde_mm_set1_ps(8.9893397e-3f));");
  ctx.println("\t\tp = simde_mm_add_ps(simde_mm_mul_ps(p, f), simde_mm_set1_ps(5.5826318e-2f));");
  ctx.println("\t\tp = simde_mm_add_ps(simde_mm_mul_ps(p, f), simde_mm_set1_ps(2.4015361e-1f));");
  ctx.println("\t\tp = simde_mm_add_ps(simde_mm_mul_ps(p, f), simde_mm_set1_ps(6.9315308e-1f));");
  ctx.println("\t\tp = simde_mm_add_ps(simde_mm_mul_ps(p, f), simde_mm_set1_ps(1.0f));");
  ctx.println("\t\tsimde__m128i exp_bits = simde_mm_slli_epi32(");
  ctx.println("\t\t\tsimde_mm_add_epi32(simde_mm_cvttps_epi32(n), simde_mm_set1_epi32(127)), 23);");
  ctx.println(
      "\t\tsimde_mm_store_ps({}.f32, "
      "simde_mm_mul_ps(p, simde_mm_castsi128_ps(exp_bits)));",
      vD);
  ctx.println("\t}}");
  return true;
}

bool build_vlogefp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  for (size_t i = 0; i < 4; i++)
    ctx.println("\t{}.f32[{}] = log2f({}.f32[{}]);", ctx.v(ctx.insn.operands[0]), i,
                ctx.v(ctx.insn.operands[1]), i);
  return true;
}

/**
 * Builds the 3 element dot product.
 *
 * The simde_mm_dp_ps mask 0xEF multiplies host elements y, z and w (high nibble, bit 7 is x down
 * to bit 4 is w) and broadcasts the sum to all 4 elements (low nibble). The host reverses the
 * guest element order, so these are the 3 guest elements of the dot product.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool build_vmsum3fp128(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_dp_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32), 0xEF));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

/**
 * Builds the 4 element dot product.
 *
 * The simde_mm_dp_ps mask 0xFF multiplies all 4 elements and broadcasts the sum to all 4.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool build_vmsum4fp128(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_dp_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32), 0xFF));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vrfim(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_unary_expr(
      "simde_mm_round_ps(simde_mm_load_ps({vA}.f32), "
      "SIMDE_MM_FROUND_TO_NEG_INF | SIMDE_MM_FROUND_NO_EXC)");
  return true;
}

bool build_vrfin(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_unary_expr(
      "simde_mm_round_ps(simde_mm_load_ps({vA}.f32), "
      "SIMDE_MM_FROUND_TO_NEAREST_INT | SIMDE_MM_FROUND_NO_EXC)");
  return true;
}

bool build_vrfip(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_unary_expr(
      "simde_mm_round_ps(simde_mm_load_ps({vA}.f32), "
      "SIMDE_MM_FROUND_TO_POS_INF | SIMDE_MM_FROUND_NO_EXC)");
  return true;
}

bool build_vrfiz(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_unary_expr(
      "simde_mm_round_ps(simde_mm_load_ps({vA}.f32), "
      "SIMDE_MM_FROUND_TO_ZERO | SIMDE_MM_FROUND_NO_EXC)");
  return true;
}

bool build_vaddsbs(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("adds_epi8", "s8");
  return true;
}

bool build_vaddshs(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("adds_epi16", "s16");
  return true;
}

/**
 * Builds vaddsws. No SSE intrinsic saturates 32 bit signed adds, so overflow is detected and
 * blended.
 *
 * Overflow is flagged by the MSB of (a ^ sum) & (b ^ sum), which is set when a and b share a sign
 * that the sum lacks. The saturated value is (a >> 31) ^ 0x7FFFFFFF, which is INT32_MAX for a
 * positive a and INT32_MIN for a negative a. The result takes it where overflow is flagged.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool build_vaddsws(BuilderContext& ctx) {
  auto vD = ctx.v(ctx.insn.operands[0]);
  auto vA = ctx.v(ctx.insn.operands[1]);
  auto vB = ctx.v(ctx.insn.operands[2]);

  ctx.println("\t{{");
  ctx.println("\t\tsimde__m128i a = simde_mm_load_si128((simde__m128i*){}.u8);", vA);
  ctx.println("\t\tsimde__m128i b = simde_mm_load_si128((simde__m128i*){}.u8);", vB);
  ctx.println("\t\tsimde__m128i sum = simde_mm_add_epi32(a, b);");
  ctx.println(
      "\t\tsimde__m128i overflow = simde_mm_and_si128(simde_mm_xor_si128(a, sum), "
      "simde_mm_xor_si128(b, sum));");
  ctx.println(
      "\t\tsimde__m128i sat_val = simde_mm_xor_si128(simde_mm_srai_epi32(a, 31), "
      "simde_mm_set1_epi32(0x7FFFFFFF));");
  ctx.println(
      "\t\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_castps_si128(simde_mm_blendv_ps("
      "simde_mm_castsi128_ps(sum), "
      "simde_mm_castsi128_ps(sat_val), "
      "simde_mm_castsi128_ps(overflow))));",
      vD);
  ctx.println("\t}}");
  return true;
}

bool build_vaddubm(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("add_epi8", "u8");
  return true;
}

bool build_vaddubs(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("adds_epu8", "u8");
  return true;
}

bool build_vadduhm(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("add_epi16", "u16");
  return true;
}

bool build_vadduwm(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("add_epi32", "u32");
  return true;
}

bool build_vadduws(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "rex::ppc::simde_mm_adds_epu32(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_load_si128((simde__m128i*){}.u32)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vadduhs(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("adds_epu16", "u16");
  return true;
}

bool build_vsubsws(BuilderContext& ctx) {
  for (size_t i = 0; i < 4; i++) {
    ctx.println("\t{}.s64 = i64({}.s32[{}]) - i64({}.s32[{}]);", ctx.temp(),
                ctx.v(ctx.insn.operands[1]), i, ctx.v(ctx.insn.operands[2]), i);
    ctx.println("\t{}.s32[{}] = {}.s64 > INT_MAX ? INT_MAX : {}.s64 < INT_MIN ? INT_MIN : {}.s64;",
                ctx.v(ctx.insn.operands[0]), i, ctx.temp(), ctx.temp(), ctx.temp());
  }
  return true;
}

bool build_vsububm(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("sub_epi8", "u8");
  return true;
}

bool build_vsububs(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("subs_epu8", "u8");
  return true;
}

bool build_vsubuws(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "simde_mm_sub_epi32(simde_mm_load_si128((simde__m128i*) {}.u32), "
      "simde_mm_min_epu32(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_load_si128((simde__m128i*){}.u32))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[1]),
      ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vsubuhs(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("subs_epu16", "u16");
  return true;
}

bool build_vsubuhm(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("sub_epi16", "u16");
  return true;
}

bool build_vsubuwm(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("sub_epi32", "u32");
  return true;
}

bool build_vmaxsw(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("max_epi32", "s32");
  return true;
}

bool build_vmaxsh(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("max_epi16", "s16");
  return true;
}

bool build_vmaxsb(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("max_epi8", "s8");
  return true;
}

bool build_vminsh(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("min_epi16", "s16");
  return true;
}

bool build_vminsb(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("min_epi8", "s8");
  return true;
}

bool build_vminsw(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("min_epi32", "s32");
  return true;
}

bool build_vmaxuh(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("max_epu16", "u16");
  return true;
}

bool build_vminuh(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("min_epu16", "u16");
  return true;
}

bool build_vminuw(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("min_epu32", "u32");
  return true;
}

bool build_vsubsbs(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("subs_epi8", "s8");
  return true;
}

bool build_vmaxub(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("max_epu8", "u8");
  return true;
}

bool build_vminub(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("min_epu8", "u8");
  return true;
}

bool build_vsubshs(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("subs_epi16", "s16");
  return true;
}

bool build_vavgsb(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_avg_epi8(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vavgsh(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u16, "
      "rex::ppc::simde_mm_avg_epi16(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_load_si128((simde__m128i*){}.u16)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vavgsw(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s32, "
      "rex::ppc::simde_mm_avg_epi32("
      "simde_mm_load_si128((simde__m128i*){}.s32), "
      "simde_mm_load_si128((simde__m128i*){}.s32)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vavgub(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("avg_epu8", "u8");
  return true;
}

bool build_vavguh(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("avg_epu16", "u16");
  return true;
}

bool build_vand(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("and_si128", "u8");
  return true;
}

bool build_vandc128(BuilderContext& ctx) {
  // simde_mm_andnot_si128 has reversed operand semantics, so vB is its first operand
  ctx.emit_vec_int_binary_swapped("andnot_si128", "u8");
  return true;
}

bool build_vandc(BuilderContext& ctx) {
  // simde_mm_andnot_si128 has reversed operand semantics, so vB is its first operand
  ctx.emit_vec_int_binary_swapped("andnot_si128", "u8");
  return true;
}

bool build_vor(BuilderContext& ctx) {
  ctx.print("\tsimde_mm_store_si128((simde__m128i*){}.u8, ", ctx.v(ctx.insn.operands[0]));

  if (ctx.insn.operands[1] != ctx.insn.operands[2])
    ctx.println(
        "simde_mm_or_si128(simde_mm_load_si128((simde__m128i*){}.u8), "
        "simde_mm_load_si128((simde__m128i*){}.u8)));",
        ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  else
    ctx.println("simde_mm_load_si128((simde__m128i*){}.u8));", ctx.v(ctx.insn.operands[1]));

  return true;
}

bool build_vxor(BuilderContext& ctx) {
  ctx.print("\tsimde_mm_store_si128((simde__m128i*){}.u8, ", ctx.v(ctx.insn.operands[0]));

  if (ctx.insn.operands[1] != ctx.insn.operands[2])
    ctx.println(
        "simde_mm_xor_si128(simde_mm_load_si128((simde__m128i*){}.u8), "
        "simde_mm_load_si128((simde__m128i*){}.u8)));",
        ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  else
    ctx.println("simde_mm_setzero_si128());");

  return true;
}

bool build_vnor(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_xor_si128("
      "simde_mm_or_si128("
      "simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)), "
      "simde_mm_set1_epi32(-1)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vsel(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_or_si128(simde_mm_andnot_si128(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)), "
      "simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[3]), ctx.v(ctx.insn.operands[1]),
      ctx.v(ctx.insn.operands[3]), ctx.v(ctx.insn.operands[2]));
  return true;
}

/**
 * Builds vcmpbfp. For each element bit 0 (0x80000000) is set when vA > vB and bit 1 (0x40000000)
 * is set when vA < -vB.
 *
 * movemask_ps reads only bit 31 while a lower bound violation sets bit 30. CR6 is therefore
 * set from vD OR'd with vD shifted left by 1 so both violations are seen.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool build_vcmpbfp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  auto vA = ctx.v(ctx.insn.operands[1]);
  auto vB = ctx.v(ctx.insn.operands[2]);
  auto vD = ctx.v(ctx.insn.operands[0]);

  ctx.println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_and_ps(simde_mm_cmpgt_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32)), simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000)))));",
      ctx.v_temp(), vA, vB);
  ctx.println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_and_ps(simde_mm_cmplt_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_xor_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000))))), "
      "simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x40000000)))));",
      vD, vA, vB);
  ctx.println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_or_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32)));",
      vD, ctx.v_temp(), vD);

  if (isRecordForm(ctx.insn))
    ctx.println(
        "\t{}.setFromMask(simde_mm_castsi128_ps(simde_mm_or_si128("
        "simde_mm_load_si128((simde__m128i*){}.f32), "
        "simde_mm_slli_epi32(simde_mm_load_si128((simde__m128i*){}.f32), 1))), 0xF);",
        ctx.cr(6), vD, vD);
  return true;
}

bool build_vcmpeqfp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_binary("cmpeq");
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_ps({}.f32), 0xF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpequb(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("cmpeq_epi8", "u8");
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_si128((simde__m128i*){}.u8), 0xFFFF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpequh(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_cmpeq_epi16(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_load_si128((simde__m128i*){}.u16)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_si128((simde__m128i*){}.u16), 0xFFFF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpequw(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_cmpeq_epi32(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_load_si128((simde__m128i*){}.u32)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_ps({}.f32), 0xF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpgefp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_binary("cmpge");
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_ps({}.f32), 0xF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpgtfp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.emit_vec_fp_binary("cmpgt");
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_ps({}.f32), 0xF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpgtub(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_cmpgt_epu8(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_si128((simde__m128i*){}.u8), 0xFFFF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpgtuh(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_cmpgt_epu16(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_load_si128((simde__m128i*){}.u16)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_si128((simde__m128i*){}.u16), 0xFFFF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpgtuw(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "simde_mm_cmpgt_epi32(simde_mm_xor_si128(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_set1_epi32((i32)0x80000000)), "
      "simde_mm_xor_si128(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_set1_epi32((i32)0x80000000))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));

  if (isRecordForm(ctx.insn))
    ctx.println(
        "\t{}.setFromMask(simde_mm_castsi128_ps(simde_mm_load_si128((simde__m128i*){}.u32)), 0xF);",
        ctx.cr(6), ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpgtsb(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("cmpgt_epi8", "u8");
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_si128((simde__m128i*){}.u8), 0xFFFF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpgtsh(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_cmpgt_epi16(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_load_si128((simde__m128i*){}.u16)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  if (isRecordForm(ctx.insn))
    ctx.println("\t{}.setFromMask(simde_mm_load_si128((simde__m128i*){}.u16), 0xFFFF);", ctx.cr(6),
                ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vcmpgtsw(BuilderContext& ctx) {
  ctx.emit_vec_int_binary("cmpgt_epi32", "u32");
  if (isRecordForm(ctx.insn))
    ctx.println(
        "\t{}.setFromMask(simde_mm_castsi128_ps(simde_mm_load_si128((simde__m128i*){}.u32)), 0xF);",
        ctx.cr(6), ctx.v(ctx.insn.operands[0]));
  return true;
}

bool build_vctsxs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.print("\tsimde_mm_store_si128((simde__m128i*){}.s32, rex::ppc::simde_mm_vctsxs(",
            ctx.v(ctx.insn.operands[0]));
  if (ctx.insn.operands[2] != 0)
    ctx.println("simde_mm_mul_ps(simde_mm_load_ps({}.f32), simde_mm_set1_ps({}))));",
                ctx.v(ctx.insn.operands[1]), 1u << ctx.insn.operands[2]);
  else
    ctx.println("simde_mm_load_ps({}.f32)));", ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vcfsx(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.print("\tsimde_mm_store_ps({}.f32, ", ctx.v(ctx.insn.operands[0]));
  if (ctx.insn.operands[2] != 0) {
    const f32 value = std::ldexp(1.0f, -static_cast<i32>(ctx.insn.operands[2]));
    ctx.println(
        "simde_mm_mul_ps(simde_mm_cvtepi32_ps(simde_mm_load_si128((simde__m128i*){}.u32)), "
        "simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x{:X})))));",
        ctx.v(ctx.insn.operands[1]), std::bit_cast<u32>(value));
  } else {
    ctx.println("simde_mm_cvtepi32_ps(simde_mm_load_si128((simde__m128i*){}.u32)));",
                ctx.v(ctx.insn.operands[1]));
  }
  return true;
}

bool build_vcfux(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.print("\tsimde_mm_store_ps({}.f32, ", ctx.v(ctx.insn.operands[0]));
  if (ctx.insn.operands[2] != 0) {
    const f32 value = std::ldexp(1.0f, -static_cast<i32>(ctx.insn.operands[2]));
    ctx.println(
        "simde_mm_mul_ps(rex::ppc::simde_mm_cvtepu32_ps_(simde_mm_load_si128((simde__m128i*){}.u32)"
        "), "
        "simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x{:X})))));",
        ctx.v(ctx.insn.operands[1]), std::bit_cast<u32>(value));
  } else {
    ctx.println("rex::ppc::simde_mm_cvtepu32_ps_(simde_mm_load_si128((simde__m128i*){}.u32)));",
                ctx.v(ctx.insn.operands[1]));
  }
  return true;
}

bool build_vctuxs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  ctx.print("\tsimde_mm_store_si128((simde__m128i*){}.u32, rex::ppc::simde_mm_vctuxs(",
            ctx.v(ctx.insn.operands[0]));
  if (ctx.insn.operands[2] != 0)
    ctx.println("simde_mm_mul_ps(simde_mm_load_ps({}.f32), simde_mm_set1_ps({}))));",
                ctx.v(ctx.insn.operands[1]), 1u << ctx.insn.operands[2]);
  else
    ctx.println("simde_mm_load_ps({}.f32)));", ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vmrghb(BuilderContext& ctx) {
  ctx.emit_vec_int_binary_swapped("unpackhi_epi8", "u8");
  return true;
}

bool build_vmrghh(BuilderContext& ctx) {
  ctx.emit_vec_int_binary_swapped("unpackhi_epi16", "u16");
  return true;
}

bool build_vmrghw(BuilderContext& ctx) {
  ctx.emit_vec_int_binary_swapped("unpackhi_epi32", "u32");
  return true;
}

bool build_vmrglb(BuilderContext& ctx) {
  ctx.emit_vec_int_binary_swapped("unpacklo_epi8", "u8");
  return true;
}

bool build_vmrglh(BuilderContext& ctx) {
  ctx.emit_vec_int_binary_swapped("unpacklo_epi16", "u16");
  return true;
}

bool build_vmrglw(BuilderContext& ctx) {
  ctx.emit_vec_int_binary_swapped("unpacklo_epi32", "u32");
  return true;
}

bool build_vperm(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_perm_epi8_(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8), simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]),
      ctx.v(ctx.insn.operands[3]));
  return true;
}

bool build_vpermwi128(BuilderContext& ctx) {
  u32 x = 3 - (ctx.insn.operands[2] & 0x3);
  u32 y = 3 - ((ctx.insn.operands[2] >> 2) & 0x3);
  u32 z = 3 - ((ctx.insn.operands[2] >> 4) & 0x3);
  u32 w = 3 - ((ctx.insn.operands[2] >> 6) & 0x3);
  u32 perm = x | (y << 2) | (z << 4) | (w << 6);
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "simde_mm_shuffle_epi32(simde_mm_load_si128((simde__m128i*){}.u32), 0x{:X}));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), perm);
  return true;
}

bool build_vrlimi128(BuilderContext& ctx) {
  constexpr size_t shuffles[] = {SIMDE_MM_SHUFFLE(3, 2, 1, 0), SIMDE_MM_SHUFFLE(2, 1, 0, 3),
                                 SIMDE_MM_SHUFFLE(1, 0, 3, 2), SIMDE_MM_SHUFFLE(0, 3, 2, 1)};
  ctx.println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_blend_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_permute_ps(simde_mm_load_ps({}.f32), {}), {}));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]),
      shuffles[ctx.insn.operands[3]], ctx.insn.operands[2]);
  return true;
}

bool build_vslb(BuilderContext& ctx) {
  ctx.emit_vec_var_shift("sllv", "epi8", 0x7);
  return true;
}

bool build_vsldoi(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_alignr_epi8(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8), {}));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]),
      16 - ctx.insn.operands[3]);
  return true;
}

bool build_vslh(BuilderContext& ctx) {
  ctx.emit_vec_var_shift("sllv", "epi16", 0xF);
  return true;
}

bool build_vsrh(BuilderContext& ctx) {
  ctx.emit_vec_var_shift("srlv", "epi16", 0xF);
  return true;
}

bool build_vsrb(BuilderContext& ctx) {
  for (size_t i = 0; i < 16; i++)
    ctx.println("\t{}.u8[{}] = {}.u8[{}] >> ({}.u8[{}] & 0x7);", ctx.v(ctx.insn.operands[0]), i,
                ctx.v(ctx.insn.operands[1]), i, ctx.v(ctx.insn.operands[2]), i);
  return true;
}

bool build_vsrab(BuilderContext& ctx) {
  for (size_t i = 0; i < 16; i++)
    ctx.println("\t{}.s8[{}] = {}.s8[{}] >> ({}.u8[{}] & 0x7);", ctx.v(ctx.insn.operands[0]), i,
                ctx.v(ctx.insn.operands[1]), i, ctx.v(ctx.insn.operands[2]), i);
  return true;
}

bool build_vsrah(BuilderContext& ctx) {
  ctx.emit_vec_var_shift("srav", "epi16", 0xF);
  return true;
}

bool build_vrlh(BuilderContext& ctx) {
  auto vD = ctx.v(ctx.insn.operands[0]);
  auto vA = ctx.v(ctx.insn.operands[1]);
  auto vB = ctx.v(ctx.insn.operands[2]);
  ctx.println("\t{{");
  ctx.println("\t\tsimde__m128i a = simde_mm_load_si128((simde__m128i*){}.u8);", vA);
  ctx.println("\t\tsimde__m128i sh = simde_mm_and_si128(");
  ctx.println("\t\t\tsimde_mm_load_si128((simde__m128i*){}.u8), simde_mm_set1_epi16(0xF));", vB);
  ctx.println("\t\tsimde__m128i rsh = simde_mm_sub_epi16(simde_mm_set1_epi16(16), sh);");
  ctx.println("\t\tsimde__m128i result = simde_mm_or_si128(");
  ctx.println("\t\t\trex::ppc::simde_mm_sllv_epi16(a, sh),");
  ctx.println("\t\t\trex::ppc::simde_mm_srlv_epi16(a, rsh));");
  ctx.println("\t\tsimde_mm_store_si128((simde__m128i*){}.u8, result);", vD);
  ctx.println("\t}}");
  return true;
}

bool build_vrlw(BuilderContext& ctx) {
  for (size_t i = 0; i < 4; i++) {
    ctx.println("\t{{ u32 sh = {}.u32[{}] & 0x1F;", ctx.v(ctx.insn.operands[2]), i);
    ctx.println("\t{}.u32[{}] = ({}.u32[{}] << sh) | (sh ? ({}.u32[{}] >> (32 - sh)) : 0); }}",
                ctx.v(ctx.insn.operands[0]), i, ctx.v(ctx.insn.operands[1]), i,
                ctx.v(ctx.insn.operands[1]), i);
  }
  return true;
}

/**
 * Builds vsl. Shifts the whole vector left by the bit count in the low 3 bits of vB.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool build_vsl(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_vsl(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

/**
 * Builds vslo. Shifts the whole vector left by the byte count in bits 121:124 of vB.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool build_vslo(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_vslo(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

/**
 * Builds vsro. Shifts the whole vector right by the byte count in bits 121:124 of vB.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool build_vsro(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_vsro(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vslw(BuilderContext& ctx) {
  auto vD = ctx.v(ctx.insn.operands[0]);
  auto vA = ctx.v(ctx.insn.operands[1]);
  auto vB = ctx.v(ctx.insn.operands[2]);
  ctx.println("\t{{");
  ctx.println("\t\tsimde__m128i a = simde_mm_load_si128((simde__m128i*){}.u8);", vA);
  ctx.println("\t\tsimde__m128i b = simde_mm_load_si128((simde__m128i*){}.u8);", vB);
  ctx.println("\t\tsimde__m128i shift = simde_mm_and_si128(b, simde_mm_set1_epi32(0x1F));");
  ctx.println(
      "\t\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_sllv_epi32(a, shift));",
      vD);
  ctx.println("\t}}");
  return true;
}

bool build_vsr(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_vsr(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[2]));
  return true;
}

bool build_vsraw(BuilderContext& ctx) {
  for (size_t i = 0; i < 4; i++)
    ctx.println("\t{}.s32[{}] = {}.s32[{}] >> ({}.u8[{}] & 0x1F);", ctx.v(ctx.insn.operands[0]), i,
                ctx.v(ctx.insn.operands[1]), i, ctx.v(ctx.insn.operands[2]), i * 4);
  return true;
}

bool build_vsrw(BuilderContext& ctx) {
  for (size_t i = 0; i < 4; i++)
    ctx.println("\t{}.u32[{}] = {}.u32[{}] >> ({}.u8[{}] & 0x1F);", ctx.v(ctx.insn.operands[0]), i,
                ctx.v(ctx.insn.operands[1]), i, ctx.v(ctx.insn.operands[2]), i * 4);
  return true;
}

bool build_vspltb(BuilderContext& ctx) {
  u32 perm = 15 - ctx.insn.operands[2];
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_set1_epi8(char(0x{:X}))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), perm);
  return true;
}

bool build_vsplth(BuilderContext& ctx) {
  u32 perm = 7 - ctx.insn.operands[2];
  perm = (perm * 2) | ((perm * 2 + 1) << 8);
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u16, "
      "simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_set1_epi16(short(0x{:X}))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), perm);
  return true;
}

bool build_vspltisb(BuilderContext& ctx) {
  i8 imm5 = static_cast<i8>(static_cast<i8>(ctx.insn.operands[1] << 3) >> 3);
  ctx.println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_set1_epi8(char(0x{:X})));",
              ctx.v(ctx.insn.operands[0]), static_cast<u8>(imm5));
  return true;
}

bool build_vspltisw(BuilderContext& ctx) {
  i8 imm5 = static_cast<i8>(static_cast<i8>(ctx.insn.operands[1] << 3) >> 3);
  ctx.println("\tsimde_mm_store_si128((simde__m128i*){}.u32, simde_mm_set1_epi32(int(0x{:X})));",
              ctx.v(ctx.insn.operands[0]), static_cast<u32>(static_cast<i32>(imm5)));
  return true;
}

bool build_vspltish(BuilderContext& ctx) {
  i8 imm5 = static_cast<i8>(static_cast<i8>(ctx.insn.operands[1] << 3) >> 3);
  ctx.println("\tsimde_mm_store_si128((simde__m128i*){}.s16, simde_mm_set1_epi16(short(0x{:X})));",
              ctx.v(ctx.insn.operands[0]), static_cast<u16>(static_cast<i16>(imm5)));
  return true;
}

bool build_vspltw(BuilderContext& ctx) {
  u32 perm = 3 - ctx.insn.operands[2];
  perm |= (perm << 2) | (perm << 4) | (perm << 6);
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "simde_mm_shuffle_epi32(simde_mm_load_si128((simde__m128i*){}.u32), 0x{:X}));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), perm);
  return true;
}

bool build_vpkshus(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_packus_epi16(simde_mm_load_si128((simde__m128i*){}.s16), "
      "simde_mm_load_si128((simde__m128i*){}.s16)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[2]), ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vpkuhum(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_packus_epi16(simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_set1_epi16(0xFF)), simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_set1_epi16(0xFF))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[2]), ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vpkuhus(BuilderContext& ctx) {
  // packus reads inputs as signed so unsigned saturation is custom, and vD may alias vA or vB
  for (size_t i = 0; i < 8; i++) {
    ctx.println("\t{}.u8[{}] = {}.u16[{}] > 0xFF ? 0xFF : (u8){}.u16[{}];", ctx.v_temp(), 15 - i,
                ctx.v(ctx.insn.operands[1]), 7 - i, ctx.v(ctx.insn.operands[1]), 7 - i);
    ctx.println("\t{}.u8[{}] = {}.u16[{}] > 0xFF ? 0xFF : (u8){}.u16[{}];", ctx.v_temp(), 7 - i,
                ctx.v(ctx.insn.operands[2]), 7 - i, ctx.v(ctx.insn.operands[2]), 7 - i);
  }
  ctx.println("\t{} = {};", ctx.v(ctx.insn.operands[0]), ctx.v_temp());
  return true;
}

bool build_vpkuwum(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u16, "
      "simde_mm_packus_epi32(simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_set1_epi32(0xFFFF)), "
      "simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_set1_epi32(0xFFFF))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[2]), ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vpkuwus(BuilderContext& ctx) {
  // packus reads inputs as signed so unsigned saturation is custom, and vD may alias vA or vB
  for (size_t i = 0; i < 4; i++) {
    ctx.println("\t{}.u16[{}] = {}.u32[{}] > 0xFFFF ? 0xFFFF : (u16){}.u32[{}];", ctx.v_temp(),
                7 - i, ctx.v(ctx.insn.operands[1]), 3 - i, ctx.v(ctx.insn.operands[1]), 3 - i);
    ctx.println("\t{}.u16[{}] = {}.u32[{}] > 0xFFFF ? 0xFFFF : (u16){}.u32[{}];", ctx.v_temp(),
                3 - i, ctx.v(ctx.insn.operands[2]), 3 - i, ctx.v(ctx.insn.operands[2]), 3 - i);
  }
  ctx.println("\t{} = {};", ctx.v(ctx.insn.operands[0]), ctx.v_temp());
  return true;
}

bool build_vpkshss(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s8, "
      "simde_mm_packs_epi16(simde_mm_load_si128((simde__m128i*){}.s16), "
      "simde_mm_load_si128((simde__m128i*){}.s16)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[2]), ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vpkswss(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s16, "
      "simde_mm_packs_epi32(simde_mm_load_si128((simde__m128i*){}.s32), "
      "simde_mm_load_si128((simde__m128i*){}.s32)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[2]), ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vpkswus(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u16, "
      "simde_mm_packus_epi32(simde_mm_load_si128((simde__m128i*){}.s32), "
      "simde_mm_load_si128((simde__m128i*){}.s32)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[2]), ctx.v(ctx.insn.operands[1]));
  return true;
}

/**
 * Builds vpkd3d128, packing the floats of vB into the format chosen by operand 2.
 *
 * Format 0 is D3DCOLOR, 1 NORMSHORT2, 2 NORMPACKED32 (w2:z10:y10:x10), 3 FLOAT16_2, 4 NORMSHORT4,
 * 5 FLOAT16_4 and 6 NORMPACKED64 (w4:z20:y20:x20). The NORM formats take floats in the 3.0 + X
 * form, so X is the integer difference from 3.0f (0x40400000), saturated and then masked to the
 * field width. NORMSHORT saturates to [-32767, 32767]. NORMPACKED32 places x at bits 0-9, y at
 * 10-19, z at 20-29 and w at 30-31. NORMPACKED64 places x at bits 0-19, y at 20-39, z at 40-59
 * and w at 60-63. D3DCOLOR clamps each channel with a 0x404000FF upper bound and tests
 * !(x >= 3.0f) so a NaN becomes the minimum.
 *
 * Guest element i is host array index 3 - i, so element 0 goes to the highest field of the
 * output. Operand 3 is the mask and operand 4 the shift. Mask 1 writes the result to
 * word[shift], mask 2 also clears word[shift + 1] and mask 3 does the same except that shift 3
 * only clears word 0.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool build_vpkd3d128(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(true);
  switch (ctx.insn.operands[2]) {
    case 0: {
      u32 mask = ctx.insn.operands[3];
      u32 shift = ctx.insn.operands[4];

      for (size_t i = 0; i < 4; i++) {
        constexpr size_t indices[] = {3, 0, 1, 2};
        ctx.println("\t{}.u32[{}] = 0x404000FF;", ctx.v_temp(), i);
        ctx.println(
            "\t{}.f32[{}] = !({}.f32[{}] >= 3.0f) ? 3.0f : ({}.f32[{}] > {}.f32[{}] ? {}.f32[{}] : "
            "{}.f32[{}]);",
            ctx.v_temp(), i, ctx.v(ctx.insn.operands[1]), i, ctx.v(ctx.insn.operands[1]), i,
            ctx.v_temp(), i, ctx.v_temp(), i, ctx.v(ctx.insn.operands[1]), i);
        ctx.println("\t{}.u32 {}= u32({}.u8[{}]) << {};", ctx.temp(), i == 0 ? "" : "|",
                    ctx.v_temp(), i * 4, indices[i] * 8);
      }

      if (mask == 3 && shift == 3) {
        ctx.println("\t{}.u32[0] = 0;", ctx.v(ctx.insn.operands[0]));
      } else {
        ctx.println("\t{}.u32[{}] = {}.u32;", ctx.v(ctx.insn.operands[0]), shift, ctx.temp());
        if (mask >= 2 && shift < 3) {
          ctx.println("\t{}.u32[{}] = 0;", ctx.v(ctx.insn.operands[0]), shift + 1);
        }
      }
      break;
    }

    case 1: {
      ctx.println("\t{}.s32 = {}.s32[3] - 0x40400000;", ctx.temp(), ctx.v(ctx.insn.operands[1]));
      ctx.println("\t{}.s32 = {}.s32 > 32767 ? 32767 : ({}.s32 < -32767 ? -32767 : {}.s32);",
                  ctx.temp(), ctx.temp(), ctx.temp(), ctx.temp());
      ctx.println("\t{}.u32[0] = u32(u16({}.s32)) << 16;", ctx.v_temp(), ctx.temp());
      ctx.println("\t{}.s32 = {}.s32[2] - 0x40400000;", ctx.temp(), ctx.v(ctx.insn.operands[1]));
      ctx.println("\t{}.s32 = {}.s32 > 32767 ? 32767 : ({}.s32 < -32767 ? -32767 : {}.s32);",
                  ctx.temp(), ctx.temp(), ctx.temp(), ctx.temp());
      ctx.println("\t{}.u32[0] |= u16({}.s32);", ctx.v_temp(), ctx.temp());
      ctx.println("\t{}.u32[{}] = {}.u32[0];", ctx.v(ctx.insn.operands[0]), ctx.insn.operands[4],
                  ctx.v_temp());
      break;
    }

    case 2: {
      ctx.println("\t{}.u32[0] = 0;", ctx.v_temp());

      ctx.println(
          "\t{}.f32 = {}.f32[3] < kPack2101010_Min10 ? kPack2101010_Min10 : ({}.f32[3] > "
          "kPack2101010_Max10 ? kPack2101010_Max10 : {}.f32[3]);",
          ctx.temp(), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[1]),
          ctx.v(ctx.insn.operands[1]));
      ctx.println("\t{}.u32[0] = {}.u32 & 0x3FF;", ctx.v_temp(), ctx.temp());

      ctx.println(
          "\t{}.f32 = {}.f32[2] < kPack2101010_Min10 ? kPack2101010_Min10 : ({}.f32[2] > "
          "kPack2101010_Max10 ? kPack2101010_Max10 : {}.f32[2]);",
          ctx.temp(), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[1]),
          ctx.v(ctx.insn.operands[1]));
      ctx.println("\t{}.u32[0] |= ({}.u32 & 0x3FF) << 10;", ctx.v_temp(), ctx.temp());

      ctx.println(
          "\t{}.f32 = {}.f32[1] < kPack2101010_Min10 ? kPack2101010_Min10 : ({}.f32[1] > "
          "kPack2101010_Max10 ? kPack2101010_Max10 : {}.f32[1]);",
          ctx.temp(), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[1]),
          ctx.v(ctx.insn.operands[1]));
      ctx.println("\t{}.u32[0] |= ({}.u32 & 0x3FF) << 20;", ctx.v_temp(), ctx.temp());

      ctx.println(
          "\t{}.f32 = {}.f32[0] < kPack2101010_Min2 ? kPack2101010_Min2 : ({}.f32[0] > "
          "kPack2101010_Max2 ? kPack2101010_Max2 : {}.f32[0]);",
          ctx.temp(), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[1]),
          ctx.v(ctx.insn.operands[1]));
      ctx.println("\t{}.u32[0] |= ({}.u32 & 0x3) << 30;", ctx.v_temp(), ctx.temp());

      ctx.println("\t{}.u32[{}] = {}.u32[0];", ctx.v(ctx.insn.operands[0]), ctx.insn.operands[4],
                  ctx.v_temp());
      break;
    }

    case 3: {
      for (size_t i = 0; i < 2; i++) {
        size_t srcIdx = 3 - i;
        size_t dstIdx = (1 - i) + (2 * static_cast<size_t>(ctx.insn.operands[4]));
        ctx.println("\t{}.u32 = ({}.u32[{}]&0x7FFFFFFF);", ctx.temp(), ctx.v(ctx.insn.operands[1]),
                    srcIdx);
        ctx.println(
            "\t{0}.u8[0] = ({1}.f32 != {1}.f32) || ({1}.f32 > 65504.0f) ? 0xFF : "
            "(({2}.u32[{3}]&0x7f800000)>>23);",
            ctx.v_temp(), ctx.temp(), ctx.v(ctx.insn.operands[1]), srcIdx);
        ctx.println("\t{}.u16 = {}.u8[0] != 0xFF ? (({}.u32[{}]&0x7FE000)>>13) : 0x0;", ctx.temp(),
                    ctx.v_temp(), ctx.v(ctx.insn.operands[1]), srcIdx);
        ctx.println(
            "\t{0}.u16[{1}] = {2}.u8[0] != 0xFF ? ({2}.u8[0] > 0x70 ? "
            "((({2}.u8[0]-0x70)<<10)+{3}.u16) : (0x71-{2}.u8[0] > 31 ? 0x0 : "
            "((0x400+{3}.u16)>>(0x71-{2}.u8[0])))) : 0x7FFF;",
            ctx.v(ctx.insn.operands[0]), dstIdx, ctx.v_temp(), ctx.temp());
        ctx.println("\t{}.u16[{}] |= (({}.u32[{}]&0x80000000)>>16);", ctx.v(ctx.insn.operands[0]),
                    dstIdx, ctx.v(ctx.insn.operands[1]), srcIdx);
      }
      break;
    }

    case 4: {
      for (size_t i = 0; i < 4; i++) {
        size_t srcIdx = 3 - i;
        size_t dstIdx = (3 - i) + (2 * static_cast<size_t>(ctx.insn.operands[4]));
        ctx.println("\t{}.s32 = {}.s32[{}] - 0x40400000;", ctx.temp(), ctx.v(ctx.insn.operands[1]),
                    srcIdx);
        ctx.println("\t{}.s32 = {}.s32 > 32767 ? 32767 : ({}.s32 < -32767 ? -32767 : {}.s32);",
                    ctx.temp(), ctx.temp(), ctx.temp(), ctx.temp());
        ctx.println("\t{}.u16[{}] = u16({}.s32);", ctx.v(ctx.insn.operands[0]), dstIdx, ctx.temp());
      }
      break;
    }

    case 5: {
      if (ctx.insn.operands[3] != 2 || ctx.insn.operands[4] > 2)
        REXLOG_WARN(codegen, "Unexpected float16_4 pack instruction at {:X}", ctx.base);

      for (size_t i = 0; i < 4; i++) {
        size_t srcIdx = 3 - i;
        size_t dstIdx = (3 - i) + (2 * static_cast<size_t>(ctx.insn.operands[4]));
        ctx.println("\t{}.u32 = ({}.u32[{}]&0x7FFFFFFF);", ctx.temp(), ctx.v(ctx.insn.operands[1]),
                    srcIdx);
        ctx.println(
            "\t{0}.u8[0] = ({1}.f32 != {1}.f32) || ({1}.f32 > 65504.0f) ? 0xFF : "
            "(({2}.u32[{3}]&0x7f800000)>>23);",
            ctx.v_temp(), ctx.temp(), ctx.v(ctx.insn.operands[1]), srcIdx);
        ctx.println("\t{}.u16 = {}.u8[0] != 0xFF ? (({}.u32[{}]&0x7FE000)>>13) : 0x0;", ctx.temp(),
                    ctx.v_temp(), ctx.v(ctx.insn.operands[1]), srcIdx);
        ctx.println(
            "\t{0}.u16[{1}] = {2}.u8[0] != 0xFF ? ({2}.u8[0] > 0x70 ? "
            "((({2}.u8[0]-0x70)<<10)+{3}.u16) : (0x71-{2}.u8[0] > 31 ? 0x0 : "
            "((0x400+{3}.u16)>>(0x71-{2}.u8[0])))) : 0x7FFF;",
            ctx.v(ctx.insn.operands[0]), dstIdx, ctx.v_temp(), ctx.temp());
        ctx.println("\t{}.u16[{}] |= (({}.u32[{}]&0x80000000)>>16);", ctx.v(ctx.insn.operands[0]),
                    dstIdx, ctx.v(ctx.insn.operands[1]), srcIdx);
      }
      break;
    }

    case 6: {
      ctx.println("\t{}.u64[0] = 0;", ctx.v_temp());
      ctx.println("\t{}.s32 = i32({}.f32[0]);", ctx.temp(), ctx.v(ctx.insn.operands[1]));
      ctx.println("\t{}.u64[0] = u64({}.s32 & 0xFFFFF);", ctx.v_temp(), ctx.temp());
      ctx.println("\t{}.s32 = i32({}.f32[1]);", ctx.temp(), ctx.v(ctx.insn.operands[1]));
      ctx.println("\t{}.u64[0] |= u64({}.s32 & 0xFFFFF) << 20;", ctx.v_temp(), ctx.temp());
      ctx.println("\t{}.s32 = i32({}.f32[2]);", ctx.temp(), ctx.v(ctx.insn.operands[1]));
      ctx.println("\t{}.u64[0] |= u64({}.s32 & 0xFFFFF) << 40;", ctx.v_temp(), ctx.temp());
      ctx.println("\t{}.s32 = i32({}.f32[3]);", ctx.temp(), ctx.v(ctx.insn.operands[1]));
      ctx.println("\t{}.u64[0] |= u64({}.s32 & 0xF) << 60;", ctx.v_temp(), ctx.temp());
      ctx.println("\t{}.u64[{}] = {}.u64[0];", ctx.v(ctx.insn.operands[0]),
                  ctx.insn.operands[4] >> 1, ctx.v_temp());
      break;
    }

    default:
      ctx.println("\t__builtin_debugtrap();");
      break;
  }
  return true;
}

/**
 * Builds vupkd3d128, unpacking vB into floats from the format chosen by operand 2 >> 2.
 *
 * The formats are numbered as for vpkd3d128. D3DCOLOR bytes are OR'd into the mantissa of 1.0f
 * (0x3F800000). The NORM formats yield the 3.0 + X form with x, y and z sign extended from 10
 * bits (NORMPACKED32) or 20 bits (NORMPACKED64), and w as 1.0 + w. NORMPACKED32 reads
 * w2:z10:y10:x10 from guest element 3 (host s32[0]) and NORMPACKED64 reads w4:z20:y20:x20.
 * FLOAT16 conversion adds the exponent bias adjustment 0x1C000 to the 0x7C00 exponent field and
 * maps a zero exponent to a signed zero. Guest element i is host array index 3 - i. The 2 and 4
 * element short and half float formats read the host u16 array from guest elements 2-3, with
 * guest element 0 taken from the highest u16 index.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool build_vupkd3d128(BuilderContext& ctx) {
  switch (ctx.insn.operands[2] >> 2) {
    case 0:
      for (size_t i = 0; i < 4; i++) {
        constexpr size_t indices[] = {3, 0, 1, 2};
        ctx.println("\t{}.u32[{}] = {}.u8[{}] | 0x3F800000;", ctx.v_temp(), i,
                    ctx.v(ctx.insn.operands[1]), indices[i]);
      }
      ctx.println("\t{} = {};", ctx.v(ctx.insn.operands[0]), ctx.v_temp());
      break;

    case 1:
      for (size_t i = 0; i < 2; i++) {
        ctx.println("\t{}.f32 = 3.0f;", ctx.temp());
        ctx.println("\t{}.s32 += {}.s16[{}];", ctx.temp(), ctx.v(ctx.insn.operands[1]), 1 - i);
        ctx.println("\t{}.f32[{}] = {}.f32;", ctx.v_temp(), 3 - i, ctx.temp());
      }
      ctx.println("\t{}.f32[1] = 0.0f;", ctx.v_temp());
      ctx.println("\t{}.f32[0] = 1.0f;", ctx.v_temp());
      ctx.println("\t{} = {};", ctx.v(ctx.insn.operands[0]), ctx.v_temp());
      break;

    case 2: {
      auto vSrc = ctx.v(ctx.insn.operands[1]);
      auto vDst = ctx.v(ctx.insn.operands[0]);
      ctx.println("\t{}.s32 = ({}.s32[0] << 22) >> 22;", ctx.temp(), vSrc);
      ctx.println("\t{}.s32[0] = {}.s32 == -512 ? 0x7FC00000 : ({}.s32 + 0x40400000);",
                  ctx.v_temp(), ctx.temp(), ctx.temp());
      ctx.println("\t{}.u32[3] = {}.u32[0];", vDst, ctx.v_temp());
      ctx.println("\t{}.s32 = ({}.s32[0] << 12) >> 22;", ctx.temp(), vSrc);
      ctx.println("\t{}.s32[0] = {}.s32 + 0x40400000;", ctx.v_temp(), ctx.temp());
      ctx.println("\t{}.u32[2] = {}.u32[0];", vDst, ctx.v_temp());
      ctx.println("\t{}.s32 = ({}.s32[0] << 2) >> 22;", ctx.temp(), vSrc);
      ctx.println("\t{}.s32[0] = {}.s32 + 0x40400000;", ctx.v_temp(), ctx.temp());
      ctx.println("\t{}.u32[1] = {}.u32[0];", vDst, ctx.v_temp());
      ctx.println("\t{}.u32[0] = ({}.u32[0] >> 30) | 0x3F800000;", ctx.v_temp(), vSrc);
      ctx.println("\t{}.u32[0] = {}.u32[0];", vDst, ctx.v_temp());
      break;
    }

    case 3: {
      auto vSrc = ctx.v(ctx.insn.operands[1]);
      auto vDst = ctx.v(ctx.insn.operands[0]);
      for (size_t i = 0; i < 2; i++) {
        size_t srcIdx = 1 - i;
        size_t dstIdx = 3 - i;
        ctx.println("\t{}.u32 = {}.u16[{}];", ctx.temp(), vSrc, srcIdx);
        ctx.println(
            "\t{}.u32[0] = (({}.u32 & 0x8000) << 16) | ((({}.u32 & 0x7C00) + 0x1C000) << 13) | "
            "(({}.u32 & 0x03FF) << 13);",
            ctx.v_temp(), ctx.temp(), ctx.temp(), ctx.temp());
        ctx.println("\tif (({}.u32 & 0x7C00) == 0) {}.u32[0] = ({}.u32 & 0x8000) << 16;",
                    ctx.temp(), ctx.v_temp(), ctx.temp());
        ctx.println("\t{}.u32[{}] = {}.u32[0];", vDst, dstIdx, ctx.v_temp());
      }
      ctx.println("\t{}.f32[1] = 0.0f;", vDst);
      ctx.println("\t{}.f32[0] = 1.0f;", vDst);
      break;
    }

    case 4: {
      for (size_t i = 0; i < 4; i++) {
        size_t srcIdx = 3 - i;
        size_t dstIdx = 3 - i;
        ctx.println("\t{}.f32 = 3.0f;", ctx.temp());
        ctx.println("\t{}.s32 += {}.s16[{}];", ctx.temp(), ctx.v(ctx.insn.operands[1]), srcIdx);
        ctx.println("\t{}.f32[{}] = {}.f32;", ctx.v(ctx.insn.operands[0]), dstIdx, ctx.temp());
      }
      break;
    }

    case 5: {
      auto vSrc = ctx.v(ctx.insn.operands[1]);
      auto vDst = ctx.v(ctx.insn.operands[0]);
      for (size_t i = 0; i < 4; i++) {
        size_t srcIdx = 3 - i;
        size_t dstIdx = 3 - i;
        ctx.println("\t{}.u32 = {}.u16[{}];", ctx.temp(), vSrc, srcIdx);
        ctx.println(
            "\t{}.u32[0] = (({}.u32 & 0x8000) << 16) | ((({}.u32 & 0x7C00) + 0x1C000) << 13) | "
            "(({}.u32 & 0x03FF) << 13);",
            ctx.v_temp(), ctx.temp(), ctx.temp(), ctx.temp());
        ctx.println("\tif (({}.u32 & 0x7C00) == 0) {}.u32[0] = ({}.u32 & 0x8000) << 16;",
                    ctx.temp(), ctx.v_temp(), ctx.temp());
        ctx.println("\t{}.u32[{}] = {}.u32[0];", vDst, dstIdx, ctx.v_temp());
      }
      break;
    }

    case 6: {
      auto vSrc = ctx.v(ctx.insn.operands[1]);
      auto vDst = ctx.v(ctx.insn.operands[0]);
      ctx.println("\t{}.u64[0] = {}.u64[1];", ctx.v_temp(), vSrc);
      ctx.println("\t{}.s32 = (i32({}.u64[0] << 44) >> 44);", ctx.temp(), ctx.v_temp());
      ctx.println("\t{}.f32[0] = f32({}.s32);", vDst, ctx.temp());
      ctx.println("\t{}.s32 = (i32({}.u64[0] << 24) >> 44);", ctx.temp(), ctx.v_temp());
      ctx.println("\t{}.f32[1] = f32({}.s32);", vDst, ctx.temp());
      ctx.println("\t{}.s32 = (i32({}.u64[0] << 4) >> 44);", ctx.temp(), ctx.v_temp());
      ctx.println("\t{}.f32[2] = f32({}.s32);", vDst, ctx.temp());
      ctx.println("\t{}.f32[3] = f32({}.u64[0] >> 60);", vDst, ctx.v_temp());
      break;
    }

    default:
      ctx.println("\t__builtin_debugtrap();");
      break;
  }
  return true;
}

bool build_vupkhsb(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s16, "
      "simde_mm_cvtepi8_epi16(simde_mm_unpackhi_epi64(simde_mm_load_si128((simde__m128i*){}.s8), "
      "simde_mm_load_si128((simde__m128i*){}.s8))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vupkhsh(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s32, "
      "simde_mm_cvtepi16_epi32(simde_mm_unpackhi_epi64(simde_mm_load_si128((simde__m128i*){}.s16), "
      "simde_mm_load_si128((simde__m128i*){}.s16))));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]), ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vupklsb(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s32, "
      "simde_mm_cvtepi8_epi16(simde_mm_load_si128((simde__m128i*){}.s16)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]));
  return true;
}

bool build_vupklsh(BuilderContext& ctx) {
  ctx.println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s32, "
      "simde_mm_cvtepi16_epi32(simde_mm_load_si128((simde__m128i*){}.s16)));",
      ctx.v(ctx.insn.operands[0]), ctx.v(ctx.insn.operands[1]));
  return true;
}

}  // namespace rex::codegen
