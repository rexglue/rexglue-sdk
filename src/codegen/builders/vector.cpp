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
#include <string_view>
#include <utility>

#include <simde/x86/sse.h>

#include <rex/logging.h>

#include <ppc.h>

namespace rex::codegen {

namespace {

/**
 * Emits the update of the VSCR saturation bit for a saturating vector add, subtract or pack. It
 * runs before the instruction so that vD may alias an operand.
 *
 * @param ctx The builder context
 * @param function The rex::ppc saturation function with its template arguments.
 */
void EmitSaturation(BuilderContext& ctx, std::string_view function) {
  ctx.Println(
      "\tctx.vscr_sat |= u8(rex::ppc::{}(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      function, ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
}

}  // namespace

bool BuildVaddfp(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPBinary("add");
  return true;
}

bool BuildVsubfp(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPBinary("sub");
  return true;
}

bool BuildVmulfp128(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPBinary("mul");
  return true;
}

bool BuildVmaddfp(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.Println(
      "\tsimde_mm_store_ps({}.f32, rex::ppc::Vmaddfp(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32), simde_mm_load_ps({}.f32), ctx.vscr_nj));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]),
      ctx.V(ctx.insn.operands[3]));
  return true;
}

/**
 * Builds vnmsubfp, vD = -(vA * vB - vC), with a single rounding.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool BuildVnmsubfp(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.Println(
      "\tsimde_mm_store_ps({}.f32, rex::ppc::Vnmsubfp(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32), simde_mm_load_ps({}.f32), ctx.vscr_nj));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]),
      ctx.V(ctx.insn.operands[3]));
  return true;
}

bool BuildVmaxfp(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPBinary("max");
  return true;
}

bool BuildVminfp(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPBinary("min");
  return true;
}

bool BuildVrefp(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPUnaryExpr("rex::ppc::Vrefp(simde_mm_load_ps({vA}.f32), ctx.vscr_nj)");
  return true;
}

bool BuildVrsqrtefp(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPUnaryExpr("rex::ppc::Vrsqrtefp(simde_mm_load_ps({vA}.f32), ctx.vscr_nj)");
  return true;
}

bool BuildVexptefp(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPUnaryExpr("rex::ppc::Vexptefp(simde_mm_load_ps({vA}.f32), ctx.vscr_nj)");
  return true;
}

bool BuildVlogefp(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPUnaryExpr("rex::ppc::Vlogefp(simde_mm_load_ps({vA}.f32), ctx.vscr_nj)");
  return true;
}

/**
 * Builds the 3 element dot product.
 *
 * The result is the bit exact dot product of vector elements 0 to 2, broadcast to all 4 elements.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool BuildVmsum3fp128(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.Println(
      "\tsimde_mm_store_ps({}.f32, rex::ppc::Vmsum3fp128(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32), ctx.vscr_nj));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

/**
 * Builds the 4 element dot product.
 *
 * The result is the bit exact dot product of all 4 vector elements, broadcast to all 4 elements.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool BuildVmsum4fp128(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.Println(
      "\tsimde_mm_store_ps({}.f32, rex::ppc::Vmsum4fp128(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32), ctx.vscr_nj));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

bool BuildVrfim(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPUnaryExpr(
      "simde_mm_round_ps(simde_mm_load_ps({vA}.f32), "
      "SIMDE_MM_FROUND_TO_NEG_INF | SIMDE_MM_FROUND_NO_EXC)");
  return true;
}

bool BuildVrfin(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPUnaryExpr(
      "simde_mm_round_ps(simde_mm_load_ps({vA}.f32), "
      "SIMDE_MM_FROUND_TO_NEAREST_INT | SIMDE_MM_FROUND_NO_EXC)");
  return true;
}

bool BuildVrfip(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPUnaryExpr(
      "simde_mm_round_ps(simde_mm_load_ps({vA}.f32), "
      "SIMDE_MM_FROUND_TO_POS_INF | SIMDE_MM_FROUND_NO_EXC)");
  return true;
}

bool BuildVrfiz(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPUnaryExpr(
      "simde_mm_round_ps(simde_mm_load_ps({vA}.f32), "
      "SIMDE_MM_FROUND_TO_ZERO | SIMDE_MM_FROUND_NO_EXC)");
  return true;
}

bool BuildVaddsbs(BuilderContext& ctx) {
  EmitSaturation(ctx, "AddSaturates<8, true>");
  ctx.EmitVecIntBinary("adds_epi8", "s8");
  return true;
}

bool BuildVaddshs(BuilderContext& ctx) {
  EmitSaturation(ctx, "AddSaturates<16, true>");
  ctx.EmitVecIntBinary("adds_epi16", "s16");
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
bool BuildVaddsws(BuilderContext& ctx) {
  EmitSaturation(ctx, "AddSaturates<32, true>");
  auto v_d = ctx.V(ctx.insn.operands[0]);
  auto v_a = ctx.V(ctx.insn.operands[1]);
  auto v_b = ctx.V(ctx.insn.operands[2]);

  ctx.Println("\t{{");
  ctx.Println("\t\tsimde__m128i a = simde_mm_load_si128((simde__m128i*){}.u8);", v_a);
  ctx.Println("\t\tsimde__m128i b = simde_mm_load_si128((simde__m128i*){}.u8);", v_b);
  ctx.Println("\t\tsimde__m128i sum = simde_mm_add_epi32(a, b);");
  ctx.Println(
      "\t\tsimde__m128i overflow = simde_mm_and_si128(simde_mm_xor_si128(a, sum), "
      "simde_mm_xor_si128(b, sum));");
  ctx.Println(
      "\t\tsimde__m128i sat_val = simde_mm_xor_si128(simde_mm_srai_epi32(a, 31), "
      "simde_mm_set1_epi32(0x7FFFFFFF));");
  ctx.Println(
      "\t\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_castps_si128(simde_mm_blendv_ps("
      "simde_mm_castsi128_ps(sum), "
      "simde_mm_castsi128_ps(sat_val), "
      "simde_mm_castsi128_ps(overflow))));",
      v_d);
  ctx.Println("\t}}");
  return true;
}

bool BuildVaddubm(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("add_epi8", "u8");
  return true;
}

bool BuildVaddubs(BuilderContext& ctx) {
  EmitSaturation(ctx, "AddSaturates<8, false>");
  ctx.EmitVecIntBinary("adds_epu8", "u8");
  return true;
}

bool BuildVadduhm(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("add_epi16", "u16");
  return true;
}

bool BuildVadduwm(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("add_epi32", "u32");
  return true;
}

bool BuildVadduws(BuilderContext& ctx) {
  EmitSaturation(ctx, "AddSaturates<32, false>");
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "rex::ppc::simde_mm_adds_epu32(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_load_si128((simde__m128i*){}.u32)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

bool BuildVadduhs(BuilderContext& ctx) {
  EmitSaturation(ctx, "AddSaturates<16, false>");
  ctx.EmitVecIntBinary("adds_epu16", "u16");
  return true;
}

bool BuildVsubsws(BuilderContext& ctx) {
  EmitSaturation(ctx, "SubSaturates<32, true>");
  for (size_t i = 0; i < 4; i++) {
    ctx.Println("\t{}.s64 = i64({}.s32[{}]) - i64({}.s32[{}]);", ctx.Temp(),
                ctx.V(ctx.insn.operands[1]), i, ctx.V(ctx.insn.operands[2]), i);
    ctx.Println("\t{}.s32[{}] = {}.s64 > INT_MAX ? INT_MAX : {}.s64 < INT_MIN ? INT_MIN : {}.s64;",
                ctx.V(ctx.insn.operands[0]), i, ctx.Temp(), ctx.Temp(), ctx.Temp());
  }
  return true;
}

bool BuildVsububm(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("sub_epi8", "u8");
  return true;
}

bool BuildVsububs(BuilderContext& ctx) {
  EmitSaturation(ctx, "SubSaturates<8, false>");
  ctx.EmitVecIntBinary("subs_epu8", "u8");
  return true;
}

bool BuildVsubuws(BuilderContext& ctx) {
  EmitSaturation(ctx, "SubSaturates<32, false>");
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "simde_mm_sub_epi32(simde_mm_load_si128((simde__m128i*) {}.u32), "
      "simde_mm_min_epu32(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_load_si128((simde__m128i*){}.u32))));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[1]),
      ctx.V(ctx.insn.operands[2]));
  return true;
}

bool BuildVsubuhs(BuilderContext& ctx) {
  EmitSaturation(ctx, "SubSaturates<16, false>");
  ctx.EmitVecIntBinary("subs_epu16", "u16");
  return true;
}

bool BuildVsubuhm(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("sub_epi16", "u16");
  return true;
}

bool BuildVsubuwm(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("sub_epi32", "u32");
  return true;
}

bool BuildVmaxsw(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("max_epi32", "s32");
  return true;
}

bool BuildVmaxsh(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("max_epi16", "s16");
  return true;
}

bool BuildVmaxsb(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("max_epi8", "s8");
  return true;
}

bool BuildVminsh(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("min_epi16", "s16");
  return true;
}

bool BuildVminsb(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("min_epi8", "s8");
  return true;
}

bool BuildVminsw(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("min_epi32", "s32");
  return true;
}

bool BuildVmaxuh(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("max_epu16", "u16");
  return true;
}

bool BuildVminuh(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("min_epu16", "u16");
  return true;
}

bool BuildVminuw(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("min_epu32", "u32");
  return true;
}

bool BuildVsubsbs(BuilderContext& ctx) {
  EmitSaturation(ctx, "SubSaturates<8, true>");
  ctx.EmitVecIntBinary("subs_epi8", "s8");
  return true;
}

bool BuildVmaxub(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("max_epu8", "u8");
  return true;
}

bool BuildVminub(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("min_epu8", "u8");
  return true;
}

bool BuildVsubshs(BuilderContext& ctx) {
  EmitSaturation(ctx, "SubSaturates<16, true>");
  ctx.EmitVecIntBinary("subs_epi16", "s16");
  return true;
}

bool BuildVavgsb(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_avg_epi8(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

bool BuildVavgsh(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u16, "
      "rex::ppc::simde_mm_avg_epi16(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_load_si128((simde__m128i*){}.u16)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

bool BuildVavgsw(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s32, "
      "rex::ppc::simde_mm_avg_epi32("
      "simde_mm_load_si128((simde__m128i*){}.s32), "
      "simde_mm_load_si128((simde__m128i*){}.s32)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

bool BuildVavgub(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("avg_epu8", "u8");
  return true;
}

bool BuildVavguh(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("avg_epu16", "u16");
  return true;
}

bool BuildVand(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("and_si128", "u8");
  return true;
}

bool BuildVandc128(BuilderContext& ctx) {
  // simde_mm_andnot_si128 has reversed operand semantics, so vB is its first operand
  ctx.EmitVecIntBinarySwapped("andnot_si128", "u8");
  return true;
}

bool BuildVandc(BuilderContext& ctx) {
  // simde_mm_andnot_si128 has reversed operand semantics, so vB is its first operand
  ctx.EmitVecIntBinarySwapped("andnot_si128", "u8");
  return true;
}

bool BuildVor(BuilderContext& ctx) {
  ctx.Print("\tsimde_mm_store_si128((simde__m128i*){}.u8, ", ctx.V(ctx.insn.operands[0]));

  if (ctx.insn.operands[1] != ctx.insn.operands[2])
    ctx.Println(
        "simde_mm_or_si128(simde_mm_load_si128((simde__m128i*){}.u8), "
        "simde_mm_load_si128((simde__m128i*){}.u8)));",
        ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  else
    ctx.Println("simde_mm_load_si128((simde__m128i*){}.u8));", ctx.V(ctx.insn.operands[1]));

  return true;
}

bool BuildVxor(BuilderContext& ctx) {
  ctx.Print("\tsimde_mm_store_si128((simde__m128i*){}.u8, ", ctx.V(ctx.insn.operands[0]));

  if (ctx.insn.operands[1] != ctx.insn.operands[2])
    ctx.Println(
        "simde_mm_xor_si128(simde_mm_load_si128((simde__m128i*){}.u8), "
        "simde_mm_load_si128((simde__m128i*){}.u8)));",
        ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  else
    ctx.Println("simde_mm_setzero_si128());");

  return true;
}

bool BuildVnor(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_xor_si128("
      "simde_mm_or_si128("
      "simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)), "
      "simde_mm_set1_epi32(-1)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

bool BuildVsel(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_or_si128(simde_mm_andnot_si128(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)), "
      "simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8))));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[3]), ctx.V(ctx.insn.operands[1]),
      ctx.V(ctx.insn.operands[3]), ctx.V(ctx.insn.operands[2]));
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
bool BuildVcmpbfp(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  auto v_a = ctx.V(ctx.insn.operands[1]);
  auto v_b = ctx.V(ctx.insn.operands[2]);
  auto v_d = ctx.V(ctx.insn.operands[0]);

  ctx.Println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_and_ps(simde_mm_cmpgt_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32)), simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000)))));",
      ctx.VTemp(), v_a, v_b);
  ctx.Println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_and_ps(simde_mm_cmplt_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_xor_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000))))), "
      "simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x40000000)))));",
      v_d, v_a, v_b);
  ctx.Println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_or_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32)));",
      v_d, ctx.VTemp(), v_d);

  if (IsRecordForm(ctx.insn))
    ctx.Println(
        "\t{}.SetFromMask(simde_mm_castsi128_ps(simde_mm_or_si128("
        "simde_mm_load_si128((simde__m128i*){}.f32), "
        "simde_mm_slli_epi32(simde_mm_load_si128((simde__m128i*){}.f32), 1))), 0xF);",
        ctx.CR(6), v_d, v_d);
  return true;
}

bool BuildVcmpeqfp(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPBinary("cmpeq");
  if (IsRecordForm(ctx.insn))
    ctx.Println("\t{}.SetFromMask(simde_mm_load_ps({}.f32), 0xF);", ctx.CR(6),
                ctx.V(ctx.insn.operands[0]));
  return true;
}

bool BuildVcmpequb(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("cmpeq_epi8", "u8");
  if (IsRecordForm(ctx.insn))
    ctx.Println("\t{}.SetFromMask(simde_mm_load_si128((simde__m128i*){}.u8), 0xFFFF);", ctx.CR(6),
                ctx.V(ctx.insn.operands[0]));
  return true;
}

bool BuildVcmpequh(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_cmpeq_epi16(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_load_si128((simde__m128i*){}.u16)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  if (IsRecordForm(ctx.insn))
    ctx.Println("\t{}.SetFromMask(simde_mm_load_si128((simde__m128i*){}.u16), 0xFFFF);", ctx.CR(6),
                ctx.V(ctx.insn.operands[0]));
  return true;
}

bool BuildVcmpequw(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_cmpeq_epi32(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_load_si128((simde__m128i*){}.u32)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  if (IsRecordForm(ctx.insn))
    ctx.Println("\t{}.SetFromMask(simde_mm_load_ps({}.f32), 0xF);", ctx.CR(6),
                ctx.V(ctx.insn.operands[0]));
  return true;
}

bool BuildVcmpgefp(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPBinary("cmpge");
  if (IsRecordForm(ctx.insn))
    ctx.Println("\t{}.SetFromMask(simde_mm_load_ps({}.f32), 0xF);", ctx.CR(6),
                ctx.V(ctx.insn.operands[0]));
  return true;
}

bool BuildVcmpgtfp(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPBinary("cmpgt");
  if (IsRecordForm(ctx.insn))
    ctx.Println("\t{}.SetFromMask(simde_mm_load_ps({}.f32), 0xF);", ctx.CR(6),
                ctx.V(ctx.insn.operands[0]));
  return true;
}

bool BuildVcmpgtub(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_cmpgt_epu8(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  if (IsRecordForm(ctx.insn))
    ctx.Println("\t{}.SetFromMask(simde_mm_load_si128((simde__m128i*){}.u8), 0xFFFF);", ctx.CR(6),
                ctx.V(ctx.insn.operands[0]));
  return true;
}

bool BuildVcmpgtuh(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_cmpgt_epu16(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_load_si128((simde__m128i*){}.u16)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  if (IsRecordForm(ctx.insn))
    ctx.Println("\t{}.SetFromMask(simde_mm_load_si128((simde__m128i*){}.u16), 0xFFFF);", ctx.CR(6),
                ctx.V(ctx.insn.operands[0]));
  return true;
}

bool BuildVcmpgtuw(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "simde_mm_cmpgt_epi32(simde_mm_xor_si128(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_set1_epi32((i32)0x80000000)), "
      "simde_mm_xor_si128(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_set1_epi32((i32)0x80000000))));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));

  if (IsRecordForm(ctx.insn))
    ctx.Println(
        "\t{}.SetFromMask(simde_mm_castsi128_ps(simde_mm_load_si128((simde__m128i*){}.u32)), 0xF);",
        ctx.CR(6), ctx.V(ctx.insn.operands[0]));
  return true;
}

bool BuildVcmpgtsb(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("cmpgt_epi8", "u8");
  if (IsRecordForm(ctx.insn))
    ctx.Println("\t{}.SetFromMask(simde_mm_load_si128((simde__m128i*){}.u8), 0xFFFF);", ctx.CR(6),
                ctx.V(ctx.insn.operands[0]));
  return true;
}

bool BuildVcmpgtsh(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_cmpgt_epi16(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_load_si128((simde__m128i*){}.u16)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  if (IsRecordForm(ctx.insn))
    ctx.Println("\t{}.SetFromMask(simde_mm_load_si128((simde__m128i*){}.u16), 0xFFFF);", ctx.CR(6),
                ctx.V(ctx.insn.operands[0]));
  return true;
}

bool BuildVcmpgtsw(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("cmpgt_epi32", "u32");
  if (IsRecordForm(ctx.insn))
    ctx.Println(
        "\t{}.SetFromMask(simde_mm_castsi128_ps(simde_mm_load_si128((simde__m128i*){}.u32)), 0xF);",
        ctx.CR(6), ctx.V(ctx.insn.operands[0]));
  return true;
}

/**
 * Builds mtvscr, keeping the saturation bit (bit 0) and the non Java bit (bit 16) of word 3 of vB.
 * While the host flush mode follows the non Java bit, the new bit is applied to the host.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool BuildMtvscr(BuilderContext& ctx) {
  ctx.Println("\tctx.vscr_sat = {}.u32[0] & 1;", ctx.V(ctx.insn.operands[0]));
  ctx.Println("\tctx.vscr_nj = ({}.u32[0] >> 16) & 1;", ctx.V(ctx.insn.operands[0]));
  if (ctx.csr_state == CSRState::kVMX) {
    ctx.Println("\tctx.fpscr.SetFlushMode(ctx.vscr_nj);");
  }
  return true;
}

/**
 * Builds mfvscr, reading the stored saturation and non Java bits into word 3 of vD and zeroing the
 * other words.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool BuildMfvscr(BuilderContext& ctx) {
  ctx.Println("\t{}.u32[0] = u32(ctx.vscr_sat) | (u32(ctx.vscr_nj) << 16);",
              ctx.V(ctx.insn.operands[0]));
  ctx.Println("\t{}.u32[1] = 0;", ctx.V(ctx.insn.operands[0]));
  ctx.Println("\t{}.u64[1] = 0;", ctx.V(ctx.insn.operands[0]));
  return true;
}

bool BuildVctsxs(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  const std::string scaled =
      ctx.insn.operands[2] != 0
          ? std::format("simde_mm_mul_ps(simde_mm_load_ps({}.f32), simde_mm_set1_ps({}))",
                        ctx.V(ctx.insn.operands[1]), 1u << ctx.insn.operands[2])
          : std::format("simde_mm_load_ps({}.f32)", ctx.V(ctx.insn.operands[1]));
  ctx.Println("\tctx.vscr_sat |= u8(rex::ppc::ConvertSaturates<true>({}));", scaled);
  ctx.Println("\tsimde_mm_store_si128((simde__m128i*){}.s32, rex::ppc::simde_mm_vctsxs({}));",
              ctx.V(ctx.insn.operands[0]), scaled);
  return true;
}

bool BuildVcfsx(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.Print("\tsimde_mm_store_ps({}.f32, ", ctx.V(ctx.insn.operands[0]));
  if (ctx.insn.operands[2] != 0) {
    const f32 value = std::ldexp(1.0f, -static_cast<i32>(ctx.insn.operands[2]));
    ctx.Println(
        "simde_mm_mul_ps(simde_mm_cvtepi32_ps(simde_mm_load_si128((simde__m128i*){}.u32)), "
        "simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x{:X})))));",
        ctx.V(ctx.insn.operands[1]), std::bit_cast<u32>(value));
  } else {
    ctx.Println("simde_mm_cvtepi32_ps(simde_mm_load_si128((simde__m128i*){}.u32)));",
                ctx.V(ctx.insn.operands[1]));
  }
  return true;
}

bool BuildVcfux(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.Print("\tsimde_mm_store_ps({}.f32, ", ctx.V(ctx.insn.operands[0]));
  if (ctx.insn.operands[2] != 0) {
    const f32 value = std::ldexp(1.0f, -static_cast<i32>(ctx.insn.operands[2]));
    ctx.Println(
        "simde_mm_mul_ps(rex::ppc::simde_mm_cvtepu32_ps_(simde_mm_load_si128((simde__m128i*){}.u32)"
        "), "
        "simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x{:X})))));",
        ctx.V(ctx.insn.operands[1]), std::bit_cast<u32>(value));
  } else {
    ctx.Println("rex::ppc::simde_mm_cvtepu32_ps_(simde_mm_load_si128((simde__m128i*){}.u32)));",
                ctx.V(ctx.insn.operands[1]));
  }
  return true;
}

bool BuildVctuxs(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  const std::string scaled =
      ctx.insn.operands[2] != 0
          ? std::format("simde_mm_mul_ps(simde_mm_load_ps({}.f32), simde_mm_set1_ps({}))",
                        ctx.V(ctx.insn.operands[1]), 1u << ctx.insn.operands[2])
          : std::format("simde_mm_load_ps({}.f32)", ctx.V(ctx.insn.operands[1]));
  ctx.Println("\tctx.vscr_sat |= u8(rex::ppc::ConvertSaturates<false>({}));", scaled);
  ctx.Println("\tsimde_mm_store_si128((simde__m128i*){}.u32, rex::ppc::simde_mm_vctuxs({}));",
              ctx.V(ctx.insn.operands[0]), scaled);
  return true;
}

bool BuildVmrghb(BuilderContext& ctx) {
  ctx.EmitVecIntBinarySwapped("unpackhi_epi8", "u8");
  return true;
}

bool BuildVmrghh(BuilderContext& ctx) {
  ctx.EmitVecIntBinarySwapped("unpackhi_epi16", "u16");
  return true;
}

bool BuildVmrghw(BuilderContext& ctx) {
  ctx.EmitVecIntBinarySwapped("unpackhi_epi32", "u32");
  return true;
}

bool BuildVmrglb(BuilderContext& ctx) {
  ctx.EmitVecIntBinarySwapped("unpacklo_epi8", "u8");
  return true;
}

bool BuildVmrglh(BuilderContext& ctx) {
  ctx.EmitVecIntBinarySwapped("unpacklo_epi16", "u16");
  return true;
}

bool BuildVmrglw(BuilderContext& ctx) {
  ctx.EmitVecIntBinarySwapped("unpacklo_epi32", "u32");
  return true;
}

bool BuildVperm(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_perm_epi8_(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8), simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]),
      ctx.V(ctx.insn.operands[3]));
  return true;
}

bool BuildVpermwi128(BuilderContext& ctx) {
  u32 x = 3 - (ctx.insn.operands[2] & 0x3);
  u32 y = 3 - ((ctx.insn.operands[2] >> 2) & 0x3);
  u32 z = 3 - ((ctx.insn.operands[2] >> 4) & 0x3);
  u32 w = 3 - ((ctx.insn.operands[2] >> 6) & 0x3);
  u32 perm = x | (y << 2) | (z << 4) | (w << 6);
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "simde_mm_shuffle_epi32(simde_mm_load_si128((simde__m128i*){}.u32), 0x{:X}));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), perm);
  return true;
}

bool BuildVrlimi128(BuilderContext& ctx) {
  constexpr size_t kShuffles[] = {SIMDE_MM_SHUFFLE(3, 2, 1, 0), SIMDE_MM_SHUFFLE(2, 1, 0, 3),
                                  SIMDE_MM_SHUFFLE(1, 0, 3, 2), SIMDE_MM_SHUFFLE(0, 3, 2, 1)};
  ctx.Println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_blend_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_permute_ps(simde_mm_load_ps({}.f32), {}), {}));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]),
      kShuffles[ctx.insn.operands[3]], ctx.insn.operands[2]);
  return true;
}

bool BuildVslb(BuilderContext& ctx) {
  ctx.EmitVecVarShift("sllv", "epi8", 0x7);
  return true;
}

bool BuildVsldoi(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_alignr_epi8(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8), {}));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]),
      16 - ctx.insn.operands[3]);
  return true;
}

bool BuildVslh(BuilderContext& ctx) {
  ctx.EmitVecVarShift("sllv", "epi16", 0xF);
  return true;
}

bool BuildVsrh(BuilderContext& ctx) {
  ctx.EmitVecVarShift("srlv", "epi16", 0xF);
  return true;
}

bool BuildVsrb(BuilderContext& ctx) {
  for (size_t i = 0; i < 16; i++)
    ctx.Println("\t{}.u8[{}] = {}.u8[{}] >> ({}.u8[{}] & 0x7);", ctx.V(ctx.insn.operands[0]), i,
                ctx.V(ctx.insn.operands[1]), i, ctx.V(ctx.insn.operands[2]), i);
  return true;
}

bool BuildVsrab(BuilderContext& ctx) {
  for (size_t i = 0; i < 16; i++)
    ctx.Println("\t{}.s8[{}] = {}.s8[{}] >> ({}.u8[{}] & 0x7);", ctx.V(ctx.insn.operands[0]), i,
                ctx.V(ctx.insn.operands[1]), i, ctx.V(ctx.insn.operands[2]), i);
  return true;
}

bool BuildVsrah(BuilderContext& ctx) {
  ctx.EmitVecVarShift("srav", "epi16", 0xF);
  return true;
}

bool BuildVrlh(BuilderContext& ctx) {
  auto v_d = ctx.V(ctx.insn.operands[0]);
  auto v_a = ctx.V(ctx.insn.operands[1]);
  auto v_b = ctx.V(ctx.insn.operands[2]);
  ctx.Println("\t{{");
  ctx.Println("\t\tsimde__m128i a = simde_mm_load_si128((simde__m128i*){}.u8);", v_a);
  ctx.Println("\t\tsimde__m128i sh = simde_mm_and_si128(");
  ctx.Println("\t\t\tsimde_mm_load_si128((simde__m128i*){}.u8), simde_mm_set1_epi16(0xF));", v_b);
  ctx.Println("\t\tsimde__m128i rsh = simde_mm_sub_epi16(simde_mm_set1_epi16(16), sh);");
  ctx.Println("\t\tsimde__m128i result = simde_mm_or_si128(");
  ctx.Println("\t\t\trex::ppc::simde_mm_sllv_epi16(a, sh),");
  ctx.Println("\t\t\trex::ppc::simde_mm_srlv_epi16(a, rsh));");
  ctx.Println("\t\tsimde_mm_store_si128((simde__m128i*){}.u8, result);", v_d);
  ctx.Println("\t}}");
  return true;
}

bool BuildVrlw(BuilderContext& ctx) {
  for (size_t i = 0; i < 4; i++) {
    ctx.Println("\t{{ u32 sh = {}.u32[{}] & 0x1F;", ctx.V(ctx.insn.operands[2]), i);
    ctx.Println("\t{}.u32[{}] = ({}.u32[{}] << sh) | (sh ? ({}.u32[{}] >> (32 - sh)) : 0); }}",
                ctx.V(ctx.insn.operands[0]), i, ctx.V(ctx.insn.operands[1]), i,
                ctx.V(ctx.insn.operands[1]), i);
  }
  return true;
}

/**
 * Builds vsl. Shifts the whole vector left by the bit count in the low 3 bits of vB.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool BuildVsl(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_vsl(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

/**
 * Builds vslo. Shifts the whole vector left by the byte count in bits 121:124 of vB.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool BuildVslo(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_vslo(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

/**
 * Builds vsro. Shifts the whole vector right by the byte count in bits 121:124 of vB.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool BuildVsro(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_vsro(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

bool BuildVslw(BuilderContext& ctx) {
  auto v_d = ctx.V(ctx.insn.operands[0]);
  auto v_a = ctx.V(ctx.insn.operands[1]);
  auto v_b = ctx.V(ctx.insn.operands[2]);
  ctx.Println("\t{{");
  ctx.Println("\t\tsimde__m128i a = simde_mm_load_si128((simde__m128i*){}.u8);", v_a);
  ctx.Println("\t\tsimde__m128i b = simde_mm_load_si128((simde__m128i*){}.u8);", v_b);
  ctx.Println("\t\tsimde__m128i shift = simde_mm_and_si128(b, simde_mm_set1_epi32(0x1F));");
  ctx.Println(
      "\t\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_sllv_epi32(a, shift));",
      v_d);
  ctx.Println("\t}}");
  return true;
}

bool BuildVsr(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simde_mm_vsr(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

bool BuildVsraw(BuilderContext& ctx) {
  for (size_t i = 0; i < 4; i++)
    ctx.Println("\t{}.s32[{}] = {}.s32[{}] >> ({}.u8[{}] & 0x1F);", ctx.V(ctx.insn.operands[0]), i,
                ctx.V(ctx.insn.operands[1]), i, ctx.V(ctx.insn.operands[2]), i * 4);
  return true;
}

bool BuildVsrw(BuilderContext& ctx) {
  for (size_t i = 0; i < 4; i++)
    ctx.Println("\t{}.u32[{}] = {}.u32[{}] >> ({}.u8[{}] & 0x1F);", ctx.V(ctx.insn.operands[0]), i,
                ctx.V(ctx.insn.operands[1]), i, ctx.V(ctx.insn.operands[2]), i * 4);
  return true;
}

bool BuildVspltb(BuilderContext& ctx) {
  u32 perm = 15 - ctx.insn.operands[2];
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_set1_epi8(char(0x{:X}))));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), perm);
  return true;
}

bool BuildVsplth(BuilderContext& ctx) {
  u32 perm = 7 - ctx.insn.operands[2];
  perm = (perm * 2) | ((perm * 2 + 1) << 8);
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u16, "
      "simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_set1_epi16(short(0x{:X}))));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), perm);
  return true;
}

bool BuildVspltisb(BuilderContext& ctx) {
  i8 imm5 = static_cast<i8>(static_cast<i8>(ctx.insn.operands[1] << 3) >> 3);
  ctx.Println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_set1_epi8(char(0x{:X})));",
              ctx.V(ctx.insn.operands[0]), static_cast<u8>(imm5));
  return true;
}

bool BuildVspltisw(BuilderContext& ctx) {
  i8 imm5 = static_cast<i8>(static_cast<i8>(ctx.insn.operands[1] << 3) >> 3);
  ctx.Println("\tsimde_mm_store_si128((simde__m128i*){}.u32, simde_mm_set1_epi32(int(0x{:X})));",
              ctx.V(ctx.insn.operands[0]), static_cast<u32>(static_cast<i32>(imm5)));
  return true;
}

bool BuildVspltish(BuilderContext& ctx) {
  i8 imm5 = static_cast<i8>(static_cast<i8>(ctx.insn.operands[1] << 3) >> 3);
  ctx.Println("\tsimde_mm_store_si128((simde__m128i*){}.s16, simde_mm_set1_epi16(short(0x{:X})));",
              ctx.V(ctx.insn.operands[0]), static_cast<u16>(static_cast<i16>(imm5)));
  return true;
}

bool BuildVspltw(BuilderContext& ctx) {
  u32 perm = 3 - ctx.insn.operands[2];
  perm |= (perm << 2) | (perm << 4) | (perm << 6);
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "simde_mm_shuffle_epi32(simde_mm_load_si128((simde__m128i*){}.u32), 0x{:X}));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), perm);
  return true;
}

bool BuildVpkshus(BuilderContext& ctx) {
  EmitSaturation(ctx, "PackSaturates<16, false>");
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_packus_epi16(simde_mm_load_si128((simde__m128i*){}.s16), "
      "simde_mm_load_si128((simde__m128i*){}.s16)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[2]), ctx.V(ctx.insn.operands[1]));
  return true;
}

/**
 * Builds vpkpx, packing the 8 words of vA and vB into 1:5:5:5 pixels.
 *
 * Each halfword takes the low bit of byte 0 and the top 5 bits of bytes 1 to 3 of its source word.
 * vA fills title halfwords 0 to 3 and vB fills 4 to 7.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool BuildVpkpx(BuilderContext& ctx) {
  for (size_t i = 0; i < 4; i++) {
    for (const auto& [src, dst] :
         {std::pair{ctx.insn.operands[1], i + 4}, std::pair{ctx.insn.operands[2], i}}) {
      ctx.Println(
          "\t{0}.u16[{1}] = u16((({2}.u32[{3}] >> 9) & 0xFC00) | (({2}.u32[{3}] >> 6) & 0x03E0) | "
          "(({2}.u32[{3}] >> 3) & 0x001F));",
          ctx.VTemp(), dst, ctx.V(src), i);
    }
  }
  ctx.Println("\t{} = {};", ctx.V(ctx.insn.operands[0]), ctx.VTemp());
  return true;
}

bool BuildVpkuhum(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_packus_epi16(simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_set1_epi16(0xFF)), simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_set1_epi16(0xFF))));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[2]), ctx.V(ctx.insn.operands[1]));
  return true;
}

bool BuildVpkuhus(BuilderContext& ctx) {
  EmitSaturation(ctx, "PackSaturates<16, false>");
  // packus reads inputs as signed so unsigned saturation is custom, and vD may alias vA or vB
  for (size_t i = 0; i < 8; i++) {
    ctx.Println("\t{}.u8[{}] = {}.u16[{}] > 0xFF ? 0xFF : (u8){}.u16[{}];", ctx.VTemp(), 15 - i,
                ctx.V(ctx.insn.operands[1]), 7 - i, ctx.V(ctx.insn.operands[1]), 7 - i);
    ctx.Println("\t{}.u8[{}] = {}.u16[{}] > 0xFF ? 0xFF : (u8){}.u16[{}];", ctx.VTemp(), 7 - i,
                ctx.V(ctx.insn.operands[2]), 7 - i, ctx.V(ctx.insn.operands[2]), 7 - i);
  }
  ctx.Println("\t{} = {};", ctx.V(ctx.insn.operands[0]), ctx.VTemp());
  return true;
}

bool BuildVpkuwum(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u16, "
      "simde_mm_packus_epi32(simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_set1_epi32(0xFFFF)), "
      "simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_set1_epi32(0xFFFF))));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[2]), ctx.V(ctx.insn.operands[1]));
  return true;
}

bool BuildVpkuwus(BuilderContext& ctx) {
  EmitSaturation(ctx, "PackSaturates<32, false>");
  // packus reads inputs as signed so unsigned saturation is custom, and vD may alias vA or vB
  for (size_t i = 0; i < 4; i++) {
    ctx.Println("\t{}.u16[{}] = {}.u32[{}] > 0xFFFF ? 0xFFFF : (u16){}.u32[{}];", ctx.VTemp(),
                7 - i, ctx.V(ctx.insn.operands[1]), 3 - i, ctx.V(ctx.insn.operands[1]), 3 - i);
    ctx.Println("\t{}.u16[{}] = {}.u32[{}] > 0xFFFF ? 0xFFFF : (u16){}.u32[{}];", ctx.VTemp(),
                3 - i, ctx.V(ctx.insn.operands[2]), 3 - i, ctx.V(ctx.insn.operands[2]), 3 - i);
  }
  ctx.Println("\t{} = {};", ctx.V(ctx.insn.operands[0]), ctx.VTemp());
  return true;
}

bool BuildVpkshss(BuilderContext& ctx) {
  EmitSaturation(ctx, "PackSaturates<16, true>");
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s8, "
      "simde_mm_packs_epi16(simde_mm_load_si128((simde__m128i*){}.s16), "
      "simde_mm_load_si128((simde__m128i*){}.s16)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[2]), ctx.V(ctx.insn.operands[1]));
  return true;
}

bool BuildVpkswss(BuilderContext& ctx) {
  EmitSaturation(ctx, "PackSaturates<32, true>");
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s16, "
      "simde_mm_packs_epi32(simde_mm_load_si128((simde__m128i*){}.s32), "
      "simde_mm_load_si128((simde__m128i*){}.s32)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[2]), ctx.V(ctx.insn.operands[1]));
  return true;
}

bool BuildVpkswus(BuilderContext& ctx) {
  EmitSaturation(ctx, "PackSaturates<32, false>");
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u16, "
      "simde_mm_packus_epi32(simde_mm_load_si128((simde__m128i*){}.s32), "
      "simde_mm_load_si128((simde__m128i*){}.s32)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[2]), ctx.V(ctx.insn.operands[1]));
  return true;
}

/**
 * Builds vpkd3d128, packing the floats of vB into the format chosen by operand 2.
 *
 * Operand 3 is the mask and operand 4 the shift. The conversion, the placement in vD and the VSCR
 * saturation bit are those of rex::ppc::PackD3D.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool BuildVpkd3d128(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  const u32 type = ctx.insn.operands[2];
  if (type > 6) {
    ctx.Println("	__builtin_debugtrap();");
    return true;
  }
  ctx.Println("	ctx.vscr_sat |= rex::ppc::PackD3D<{}, {}, {}>({}, {});", type, ctx.insn.operands[3],
              ctx.insn.operands[4], ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]));
  return true;
}

/**
 * Builds vupkd3d128, unpacking vB into floats from the format chosen by operand 2 >> 2.
 *
 * The formats are numbered as for vpkd3d128. D3DCOLOR bytes are OR'd into the mantissa of 1.0f
 * (0x3F800000). The SHORT and NORM formats yield 0x40400000 plus the field sign extended from 16
 * bits (SHORT2, SHORT4), 10 bits (NORMPACKED32) or 20 bits (NORMPACKED64), and a field at its
 * minimum gives the quiet NaN 0x7FC00000. The NORM formats give w as 0x3F800000 OR the w bits.
 * NORMPACKED32 reads w2:z10:y10:x10 from guest element 3 (host s32[0]) and NORMPACKED64 reads
 * w4:z20:y20:x20 from title elements 2 and 3 (host u64[0]).
 * FLOAT16 conversion adds the exponent bias adjustment 0x1C000 to the 0x7C00 exponent field and
 * maps a zero exponent to a signed zero. Guest element i is host array index 3 - i. The 2 and 4
 * element short and half float formats read the host u16 array from guest elements 2-3, with
 * guest element 0 taken from the highest u16 index.
 *
 * @param ctx The builder context
 * @return True when the instruction is handled.
 */
bool BuildVupkd3d128(BuilderContext& ctx) {
  switch (ctx.insn.operands[2] >> 2) {
    case 0:
      for (size_t i = 0; i < 4; i++) {
        constexpr size_t kIndices[] = {3, 0, 1, 2};
        ctx.Println("\t{}.u32[{}] = {}.u8[{}] | 0x3F800000;", ctx.VTemp(), i,
                    ctx.V(ctx.insn.operands[1]), kIndices[i]);
      }
      ctx.Println("\t{} = {};", ctx.V(ctx.insn.operands[0]), ctx.VTemp());
      break;

    case 1:
      for (size_t i = 0; i < 2; i++) {
        auto v_src = ctx.V(ctx.insn.operands[1]);
        ctx.Println("\t{}.s32[{}] = {}.s16[{}] == -32768 ? 0x7FC00000 : 0x40400000 + {}.s16[{}];",
                    ctx.VTemp(), 3 - i, v_src, 1 - i, v_src, 1 - i);
      }
      ctx.Println("\t{}.f32[1] = 0.0f;", ctx.VTemp());
      ctx.Println("\t{}.f32[0] = 1.0f;", ctx.VTemp());
      ctx.Println("\t{} = {};", ctx.V(ctx.insn.operands[0]), ctx.VTemp());
      break;

    case 2: {
      auto v_src = ctx.V(ctx.insn.operands[1]);
      auto v_dst = ctx.V(ctx.insn.operands[0]);
      constexpr std::pair<i32, size_t> kFields[] = {{22, 3}, {12, 2}, {2, 1}};
      for (const auto& [shift, dst_idx] : kFields) {
        ctx.Println("\t{}.s32 = ({}.s32[0] << {}) >> 22;", ctx.Temp(), v_src, shift);
        ctx.Println("\t{}.s32[{}] = {}.s32 == -512 ? 0x7FC00000 : {}.s32 + 0x40400000;",
                    ctx.VTemp(), dst_idx, ctx.Temp(), ctx.Temp());
      }
      ctx.Println("\t{}.u32[0] = ({}.u32[0] >> 30) | 0x3F800000;", ctx.VTemp(), v_src);
      ctx.Println("\t{} = {};", v_dst, ctx.VTemp());
      break;
    }

    case 3: {
      auto v_src = ctx.V(ctx.insn.operands[1]);
      auto v_dst = ctx.V(ctx.insn.operands[0]);
      for (size_t i = 0; i < 2; i++) {
        size_t src_idx = 1 - i;
        size_t dst_idx = 3 - i;
        ctx.Println("\t{}.u32 = {}.u16[{}];", ctx.Temp(), v_src, src_idx);
        ctx.Println(
            "\t{}.u32[0] = (({}.u32 & 0x8000) << 16) | ((({}.u32 & 0x7C00) + 0x1C000) << 13) | "
            "(({}.u32 & 0x03FF) << 13);",
            ctx.VTemp(), ctx.Temp(), ctx.Temp(), ctx.Temp());
        ctx.Println("\tif (({}.u32 & 0x7C00) == 0) {}.u32[0] = ({}.u32 & 0x8000) << 16;",
                    ctx.Temp(), ctx.VTemp(), ctx.Temp());
        ctx.Println("\t{}.u32[{}] = {}.u32[0];", v_dst, dst_idx, ctx.VTemp());
      }
      ctx.Println("\t{}.f32[1] = 0.0f;", v_dst);
      ctx.Println("\t{}.f32[0] = 1.0f;", v_dst);
      break;
    }

    case 4: {
      auto v_src = ctx.V(ctx.insn.operands[1]);
      for (size_t i = 0; i < 4; i++) {
        ctx.Println("\t{}.s32[{}] = {}.s16[{}] == -32768 ? 0x7FC00000 : 0x40400000 + {}.s16[{}];",
                    ctx.VTemp(), 3 - i, v_src, 3 - i, v_src, 3 - i);
      }
      ctx.Println("\t{} = {};", ctx.V(ctx.insn.operands[0]), ctx.VTemp());
      break;
    }

    case 5: {
      auto v_src = ctx.V(ctx.insn.operands[1]);
      auto v_dst = ctx.V(ctx.insn.operands[0]);
      for (size_t i = 0; i < 4; i++) {
        size_t src_idx = 3 - i;
        size_t dst_idx = 3 - i;
        ctx.Println("\t{}.u32 = {}.u16[{}];", ctx.Temp(), v_src, src_idx);
        ctx.Println(
            "\t{}.u32[0] = (({}.u32 & 0x8000) << 16) | ((({}.u32 & 0x7C00) + 0x1C000) << 13) | "
            "(({}.u32 & 0x03FF) << 13);",
            ctx.VTemp(), ctx.Temp(), ctx.Temp(), ctx.Temp());
        ctx.Println("\tif (({}.u32 & 0x7C00) == 0) {}.u32[0] = ({}.u32 & 0x8000) << 16;",
                    ctx.Temp(), ctx.VTemp(), ctx.Temp());
        ctx.Println("\t{}.u32[{}] = {}.u32[0];", v_dst, dst_idx, ctx.VTemp());
      }
      break;
    }

    case 6: {
      auto v_src = ctx.V(ctx.insn.operands[1]);
      auto v_dst = ctx.V(ctx.insn.operands[0]);
      constexpr std::pair<i32, size_t> kFields[] = {{44, 3}, {24, 2}, {4, 1}};
      for (const auto& [shift, dst_idx] : kFields) {
        ctx.Println("\t{}.s32 = i32(i64({}.u64[0] << {}) >> 44);", ctx.Temp(), v_src, shift);
        ctx.Println("\t{}.s32[{}] = {}.s32 == -524288 ? 0x7FC00000 : {}.s32 + 0x40400000;",
                    ctx.VTemp(), dst_idx, ctx.Temp(), ctx.Temp());
      }
      ctx.Println("\t{}.u32[0] = u32({}.u64[0] >> 60) | 0x3F800000;", ctx.VTemp(), v_src);
      ctx.Println("\t{} = {};", v_dst, ctx.VTemp());
      break;
    }

    default:
      ctx.Println("\t__builtin_debugtrap();");
      break;
  }
  return true;
}

bool BuildVupkhsb(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s16, "
      "simde_mm_cvtepi8_epi16(simde_mm_unpackhi_epi64(simde_mm_load_si128((simde__m128i*){}.s8), "
      "simde_mm_load_si128((simde__m128i*){}.s8))));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[1]));
  return true;
}

bool BuildVupkhsh(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s32, "
      "simde_mm_cvtepi16_epi32(simde_mm_unpackhi_epi64(simde_mm_load_si128((simde__m128i*){}.s16), "
      "simde_mm_load_si128((simde__m128i*){}.s16))));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[1]));
  return true;
}

bool BuildVupklsb(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s32, "
      "simde_mm_cvtepi8_epi16(simde_mm_load_si128((simde__m128i*){}.s16)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]));
  return true;
}

bool BuildVupklsh(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s32, "
      "simde_mm_cvtepi16_epi32(simde_mm_load_si128((simde__m128i*){}.s16)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]));
  return true;
}

}  // namespace rex::codegen
