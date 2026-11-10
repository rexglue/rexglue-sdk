/**
 * @file        codegen/builder/vector.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "builder.h"

#include <bit>
#include <cmath>
#include <string_view>
#include <utility>

#include <simde/x86/sse.h>

#include <rex/logging/log_category.h>

#include <ppc.h>

#include <dis-asm.h>
#include <ppc-inst.h>

namespace rex::codegen {

namespace {

/**
 * Emits the VSCR saturation bit update of a saturating add, subtract or pack. It runs before the
 * instruction so that vD may alias an operand.
 *
 * @param function The rex::ppc::simd saturation function with its template arguments
 */
void EmitSaturation(BuilderContext& ctx, std::string_view function) {
  ctx.Println(
      "\tctx.vscr.sat |= u8(rex::ppc::simd::{}(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      function, ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
}

}  // namespace

template <>
bool Build<PPC_INST_VADDFP>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPBinary("add");
  return true;
}

template <>
bool Build<PPC_INST_VSUBFP>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPBinary("sub");
  return true;
}

template <>
bool Build<PPC_INST_VMULFP128>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPBinary("mul");
  return true;
}

template <>
bool Build<PPC_INST_VMADDFP>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  if (ctx.GetConfig().vector_math.vmaddfp == VectorMath::kFast) {
    ctx.Println(
        "\tsimde_mm_store_ps({}.f32, simde_mm_add_ps(simde_mm_mul_ps(simde_mm_load_ps({}.f32), "
        "simde_mm_load_ps({}.f32)), simde_mm_load_ps({}.f32)));",
        ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]),
        ctx.V(ctx.insn.operands[3]));
    return true;
  }
  ctx.Println(
      "\tsimde_mm_store_ps({}.f32, rex::ppc::simd::MultiplyAdd(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32), simde_mm_load_ps({}.f32), ctx.vscr.nj));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]),
      ctx.V(ctx.insn.operands[3]));
  return true;
}

/** Builds vnmsubfp, vD = -(vA * vB - vC), with a single rounding. */
template <>
bool Build<PPC_INST_VNMSUBFP>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  if (ctx.GetConfig().vector_math.vnmsubfp == VectorMath::kFast) {
    ctx.Println(
        "\tsimde_mm_store_ps({}.f32, "
        "simde_mm_xor_ps(simde_mm_sub_ps(simde_mm_mul_ps(simde_mm_load_ps({}.f32), "
        "simde_mm_load_ps({}.f32)), simde_mm_load_ps({}.f32)), "
        "simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000)))));",
        ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]),
        ctx.V(ctx.insn.operands[3]));
    return true;
  }
  ctx.Println(
      "\tsimde_mm_store_ps({}.f32, "
      "rex::ppc::simd::NegativeMultiplySubtract(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32), simde_mm_load_ps({}.f32), ctx.vscr.nj));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]),
      ctx.V(ctx.insn.operands[3]));
  return true;
}

template <>
bool Build<PPC_INST_VMAXFP>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPBinary("max");
  return true;
}

template <>
bool Build<PPC_INST_VMINFP>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPBinary("min");
  return true;
}

template <>
bool Build<PPC_INST_VREFP>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  if (ctx.GetConfig().vector_math.vrefp == VectorMath::kFast) {
    ctx.EmitVecFPUnaryExpr("simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_load_ps({vA}.f32))");
    return true;
  }
  ctx.EmitVecFPUnaryExpr(
      "rex::ppc::simd::ReciprocalEstimate(simde_mm_load_ps({vA}.f32), ctx.vscr.nj)");
  return true;
}

template <>
bool Build<PPC_INST_VRSQRTEFP>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  if (ctx.GetConfig().vector_math.vrsqrtefp == VectorMath::kFast) {
    ctx.EmitVecFPUnaryExpr(
        "simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_sqrt_ps(simde_mm_load_ps({vA}.f32)))");
    return true;
  }
  ctx.EmitVecFPUnaryExpr(
      "rex::ppc::simd::ReciprocalSqrtEstimate(simde_mm_load_ps({vA}.f32), ctx.vscr.nj)");
  return true;
}

template <>
bool Build<PPC_INST_VEXPTEFP>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  if (ctx.GetConfig().vector_math.vexptefp == VectorMath::kFast) {
    ctx.Println("\t{{");
    ctx.Println("\t\tsimde__m128 x = simde_mm_load_ps({}.f32);", ctx.V(ctx.insn.operands[1]));
    ctx.Println(
        "\t\tsimde__m128 n = simde_mm_round_ps(x, "
        "SIMDE_MM_FROUND_TO_NEG_INF | SIMDE_MM_FROUND_NO_EXC);");
    ctx.Println("\t\tsimde__m128 f = simde_mm_sub_ps(x, n);");
    ctx.Println("\t\tsimde__m128 p = simde_mm_set1_ps(1.8775767e-3f);");
    for (std::string_view coefficient :
         {"8.9893397e-3f", "5.5826318e-2f", "2.4015361e-1f", "6.9315308e-1f", "1.0f"}) {
      ctx.Println("\t\tp = simde_mm_add_ps(simde_mm_mul_ps(p, f), simde_mm_set1_ps({}));",
                  coefficient);
    }
    ctx.Println(
        "\t\tsimde__m128i e = simde_mm_slli_epi32(simde_mm_add_epi32(simde_mm_cvttps_epi32(n), "
        "simde_mm_set1_epi32(127)), 23);");
    ctx.Println("\t\tsimde_mm_store_ps({}.f32, simde_mm_mul_ps(p, simde_mm_castsi128_ps(e)));",
                ctx.V(ctx.insn.operands[0]));
    ctx.Println("\t}}");
    return true;
  }
  ctx.EmitVecFPUnaryExpr("rex::ppc::simd::Exp2Estimate(simde_mm_load_ps({vA}.f32), ctx.vscr.nj)");
  return true;
}

template <>
bool Build<PPC_INST_VLOGEFP>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPUnaryExpr("rex::ppc::simd::Log2Estimate(simde_mm_load_ps({vA}.f32), ctx.vscr.nj)");
  return true;
}

/** Builds the bit exact dot product of elements 0 to 2, broadcast to all four elements. */
template <>
bool Build<PPC_INST_VMSUM3FP128>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  if (ctx.GetConfig().vector_math.vmsum3fp128 == VectorMath::kFast) {
    ctx.Println(
        "\tsimde_mm_store_ps({}.f32, simde_mm_dp_ps(simde_mm_load_ps({}.f32), "
        "simde_mm_load_ps({}.f32), 0xEF));",
        ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
    return true;
  }
  ctx.Println(
      "\tsimde_mm_store_ps({}.f32, rex::ppc::simd::DotProduct3(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32), ctx.vscr.nj));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

/** Builds the bit exact dot product of all four elements, broadcast to all four elements. */
template <>
bool Build<PPC_INST_VMSUM4FP128>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  if (ctx.GetConfig().vector_math.vmsum4fp128 == VectorMath::kFast) {
    ctx.Println(
        "\tsimde_mm_store_ps({}.f32, simde_mm_dp_ps(simde_mm_load_ps({}.f32), "
        "simde_mm_load_ps({}.f32), 0xFF));",
        ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
    return true;
  }
  ctx.Println(
      "\tsimde_mm_store_ps({}.f32, rex::ppc::simd::DotProduct4(simde_mm_load_ps({}.f32), "
      "simde_mm_load_ps({}.f32), ctx.vscr.nj));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_VRFIM>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPUnaryExpr(
      "simde_mm_round_ps(simde_mm_load_ps({vA}.f32), "
      "SIMDE_MM_FROUND_TO_NEG_INF | SIMDE_MM_FROUND_NO_EXC)");
  return true;
}

template <>
bool Build<PPC_INST_VRFIN>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPUnaryExpr(
      "simde_mm_round_ps(simde_mm_load_ps({vA}.f32), "
      "SIMDE_MM_FROUND_TO_NEAREST_INT | SIMDE_MM_FROUND_NO_EXC)");
  return true;
}

template <>
bool Build<PPC_INST_VRFIP>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPUnaryExpr(
      "simde_mm_round_ps(simde_mm_load_ps({vA}.f32), "
      "SIMDE_MM_FROUND_TO_POS_INF | SIMDE_MM_FROUND_NO_EXC)");
  return true;
}

template <>
bool Build<PPC_INST_VRFIZ>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPUnaryExpr(
      "simde_mm_round_ps(simde_mm_load_ps({vA}.f32), "
      "SIMDE_MM_FROUND_TO_ZERO | SIMDE_MM_FROUND_NO_EXC)");
  return true;
}

template <>
bool Build<PPC_INST_VADDSBS>(BuilderContext& ctx) {
  EmitSaturation(ctx, "IsAddSaturating<8, true>");
  ctx.EmitVecIntBinary("adds_epi8", "s8");
  return true;
}

template <>
bool Build<PPC_INST_VADDSHS>(BuilderContext& ctx) {
  EmitSaturation(ctx, "IsAddSaturating<16, true>");
  ctx.EmitVecIntBinary("adds_epi16", "s16");
  return true;
}

/**
 * Builds vaddsws. No SSE intrinsic saturates 32 bit signed adds, so overflow is detected through
 * the MSB of (a ^ sum) & (b ^ sum) and blended with (a >> 31) ^ 0x7FFFFFFF.
 */
template <>
bool Build<PPC_INST_VADDSWS>(BuilderContext& ctx) {
  EmitSaturation(ctx, "IsAddSaturating<32, true>");
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

template <>
bool Build<PPC_INST_VADDUBM>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("add_epi8", "u8");
  return true;
}

template <>
bool Build<PPC_INST_VADDUBS>(BuilderContext& ctx) {
  EmitSaturation(ctx, "IsAddSaturating<8, false>");
  ctx.EmitVecIntBinary("adds_epu8", "u8");
  return true;
}

template <>
bool Build<PPC_INST_VADDUHM>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("add_epi16", "u16");
  return true;
}

template <>
bool Build<PPC_INST_VADDUWM>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("add_epi32", "u32");
  return true;
}

template <>
bool Build<PPC_INST_VADDUWS>(BuilderContext& ctx) {
  EmitSaturation(ctx, "IsAddSaturating<32, false>");
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "rex::ppc::simd::AddSaturate<u32>(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_load_si128((simde__m128i*){}.u32)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_VADDUHS>(BuilderContext& ctx) {
  EmitSaturation(ctx, "IsAddSaturating<16, false>");
  ctx.EmitVecIntBinary("adds_epu16", "u16");
  return true;
}

template <>
bool Build<PPC_INST_VSUBSWS>(BuilderContext& ctx) {
  EmitSaturation(ctx, "IsSubSaturating<32, true>");
  for (size_t i = 0; i < 4; i++) {
    ctx.Println("\t{}.s64 = i64({}.s32[{}]) - i64({}.s32[{}]);", ctx.Temp(),
                ctx.V(ctx.insn.operands[1]), i, ctx.V(ctx.insn.operands[2]), i);
    ctx.Println("\t{}.s32[{}] = {}.s64 > INT_MAX ? INT_MAX : {}.s64 < INT_MIN ? INT_MIN : {}.s64;",
                ctx.V(ctx.insn.operands[0]), i, ctx.Temp(), ctx.Temp(), ctx.Temp());
  }
  return true;
}

template <>
bool Build<PPC_INST_VSUBUBM>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("sub_epi8", "u8");
  return true;
}

template <>
bool Build<PPC_INST_VSUBUBS>(BuilderContext& ctx) {
  EmitSaturation(ctx, "IsSubSaturating<8, false>");
  ctx.EmitVecIntBinary("subs_epu8", "u8");
  return true;
}

template <>
bool Build<PPC_INST_VSUBUWS>(BuilderContext& ctx) {
  EmitSaturation(ctx, "IsSubSaturating<32, false>");
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "simde_mm_sub_epi32(simde_mm_load_si128((simde__m128i*) {}.u32), "
      "simde_mm_min_epu32(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_load_si128((simde__m128i*){}.u32))));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[1]),
      ctx.V(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_VSUBUHS>(BuilderContext& ctx) {
  EmitSaturation(ctx, "IsSubSaturating<16, false>");
  ctx.EmitVecIntBinary("subs_epu16", "u16");
  return true;
}

template <>
bool Build<PPC_INST_VSUBUHM>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("sub_epi16", "u16");
  return true;
}

template <>
bool Build<PPC_INST_VSUBUWM>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("sub_epi32", "u32");
  return true;
}

template <>
bool Build<PPC_INST_VMAXSW>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("max_epi32", "s32");
  return true;
}

template <>
bool Build<PPC_INST_VMAXSH>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("max_epi16", "s16");
  return true;
}

template <>
bool Build<PPC_INST_VMAXSB>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("max_epi8", "s8");
  return true;
}

template <>
bool Build<PPC_INST_VMINSH>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("min_epi16", "s16");
  return true;
}

template <>
bool Build<PPC_INST_VMINSB>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("min_epi8", "s8");
  return true;
}

template <>
bool Build<PPC_INST_VMINSW>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("min_epi32", "s32");
  return true;
}

template <>
bool Build<PPC_INST_VMAXUH>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("max_epu16", "u16");
  return true;
}

template <>
bool Build<PPC_INST_VMAXUW>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("max_epu32", "u32");
  return true;
}

template <>
bool Build<PPC_INST_VMINUH>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("min_epu16", "u16");
  return true;
}

template <>
bool Build<PPC_INST_VMINUW>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("min_epu32", "u32");
  return true;
}

template <>
bool Build<PPC_INST_VSUBSBS>(BuilderContext& ctx) {
  EmitSaturation(ctx, "IsSubSaturating<8, true>");
  ctx.EmitVecIntBinary("subs_epi8", "s8");
  return true;
}

template <>
bool Build<PPC_INST_VMAXUB>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("max_epu8", "u8");
  return true;
}

template <>
bool Build<PPC_INST_VMINUB>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("min_epu8", "u8");
  return true;
}

template <>
bool Build<PPC_INST_VSUBSHS>(BuilderContext& ctx) {
  EmitSaturation(ctx, "IsSubSaturating<16, true>");
  ctx.EmitVecIntBinary("subs_epi16", "s16");
  return true;
}

template <>
bool Build<PPC_INST_VAVGSB>(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simd::Average<i8>(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_VAVGSH>(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u16, "
      "rex::ppc::simd::Average<i16>(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_load_si128((simde__m128i*){}.u16)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_VAVGSW>(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s32, "
      "rex::ppc::simd::Average<i32>("
      "simde_mm_load_si128((simde__m128i*){}.s32), "
      "simde_mm_load_si128((simde__m128i*){}.s32)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_VAVGUB>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("avg_epu8", "u8");
  return true;
}

template <>
bool Build<PPC_INST_VAVGUH>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("avg_epu16", "u16");
  return true;
}

template <>
bool Build<PPC_INST_VAND>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("and_si128", "u8");
  return true;
}

template <>
bool Build<PPC_INST_VANDC128>(BuilderContext& ctx) {
  // simde_mm_andnot_si128 has reversed operand semantics, so vB is its first operand
  ctx.EmitVecIntBinarySwapped("andnot_si128", "u8");
  return true;
}

template <>
bool Build<PPC_INST_VANDC>(BuilderContext& ctx) {
  // simde_mm_andnot_si128 has reversed operand semantics, so vB is its first operand
  ctx.EmitVecIntBinarySwapped("andnot_si128", "u8");
  return true;
}

template <>
bool Build<PPC_INST_VOR>(BuilderContext& ctx) {
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

template <>
bool Build<PPC_INST_VXOR>(BuilderContext& ctx) {
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

template <>
bool Build<PPC_INST_VNOR>(BuilderContext& ctx) {
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

template <>
bool Build<PPC_INST_VSEL>(BuilderContext& ctx) {
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
 * Builds vcmpbfp. Per element, 0x80000000 is set when vA > vB and 0x40000000 when vA < -vB. CR6
 * is set from vD OR'd with vD shifted left by 1, because movemask_ps reads only bit 31.
 */
template <>
bool Build<PPC_INST_VCMPBFP>(BuilderContext& ctx) {
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

template <>
bool Build<PPC_INST_VCMPEQFP>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPBinary("cmpeq");
  if (IsRecordForm(ctx.insn))
    ctx.Println("\t{}.SetFromMask(simde_mm_load_ps({}.f32), 0xF);", ctx.CR(6),
                ctx.V(ctx.insn.operands[0]));
  return true;
}

template <>
bool Build<PPC_INST_VCMPEQUB>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("cmpeq_epi8", "u8");
  if (IsRecordForm(ctx.insn))
    ctx.Println("\t{}.SetFromMask(simde_mm_load_si128((simde__m128i*){}.u8), 0xFFFF);", ctx.CR(6),
                ctx.V(ctx.insn.operands[0]));
  return true;
}

template <>
bool Build<PPC_INST_VCMPEQUH>(BuilderContext& ctx) {
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

template <>
bool Build<PPC_INST_VCMPEQUW>(BuilderContext& ctx) {
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

template <>
bool Build<PPC_INST_VCMPGEFP>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPBinary("cmpge");
  if (IsRecordForm(ctx.insn))
    ctx.Println("\t{}.SetFromMask(simde_mm_load_ps({}.f32), 0xF);", ctx.CR(6),
                ctx.V(ctx.insn.operands[0]));
  return true;
}

template <>
bool Build<PPC_INST_VCMPGTFP>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.EmitVecFPBinary("cmpgt");
  if (IsRecordForm(ctx.insn))
    ctx.Println("\t{}.SetFromMask(simde_mm_load_ps({}.f32), 0xF);", ctx.CR(6),
                ctx.V(ctx.insn.operands[0]));
  return true;
}

template <>
bool Build<PPC_INST_VCMPGTUB>(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simd::CompareGreater<u8>(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  if (IsRecordForm(ctx.insn))
    ctx.Println("\t{}.SetFromMask(simde_mm_load_si128((simde__m128i*){}.u8), 0xFFFF);", ctx.CR(6),
                ctx.V(ctx.insn.operands[0]));
  return true;
}

template <>
bool Build<PPC_INST_VCMPGTUH>(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simd::CompareGreater<u16>(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_load_si128((simde__m128i*){}.u16)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  if (IsRecordForm(ctx.insn))
    ctx.Println("\t{}.SetFromMask(simde_mm_load_si128((simde__m128i*){}.u16), 0xFFFF);", ctx.CR(6),
                ctx.V(ctx.insn.operands[0]));
  return true;
}

template <>
bool Build<PPC_INST_VCMPGTUW>(BuilderContext& ctx) {
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

template <>
bool Build<PPC_INST_VCMPGTSB>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("cmpgt_epi8", "u8");
  if (IsRecordForm(ctx.insn))
    ctx.Println("\t{}.SetFromMask(simde_mm_load_si128((simde__m128i*){}.u8), 0xFFFF);", ctx.CR(6),
                ctx.V(ctx.insn.operands[0]));
  return true;
}

template <>
bool Build<PPC_INST_VCMPGTSH>(BuilderContext& ctx) {
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

template <>
bool Build<PPC_INST_VCMPGTSW>(BuilderContext& ctx) {
  ctx.EmitVecIntBinary("cmpgt_epi32", "u32");
  if (IsRecordForm(ctx.insn))
    ctx.Println(
        "\t{}.SetFromMask(simde_mm_castsi128_ps(simde_mm_load_si128((simde__m128i*){}.u32)), 0xF);",
        ctx.CR(6), ctx.V(ctx.insn.operands[0]));
  return true;
}

/**
 * Builds mtvscr, keeping the saturation bit (bit 0) and the non Java bit (bit 16) of word 3 of vB.
 * The host flush mode then follows the new non Java bit.
 */
template <>
bool Build<PPC_INST_MTVSCR>(BuilderContext& ctx) {
  ctx.Println("\tctx.vscr.Write({}.u32[0]);", ctx.V(ctx.insn.operands[0]));
  if (ctx.csr_state == CSRState::kVMX) {
    ctx.Println("\tctx.fpscr.SetFlushMode(ctx.vscr.nj);");
  }
  return true;
}

/**
 * Builds mfvscr, which reads the saturation and non Java bits into word 3 of vD and zeroes the
 * rest.
 */
template <>
bool Build<PPC_INST_MFVSCR>(BuilderContext& ctx) {
  ctx.Println("\t{}.u32[0] = ctx.vscr.Read();", ctx.V(ctx.insn.operands[0]));
  ctx.Println("\t{}.u32[1] = 0;", ctx.V(ctx.insn.operands[0]));
  ctx.Println("\t{}.u64[1] = 0;", ctx.V(ctx.insn.operands[0]));
  return true;
}

template <>
bool Build<PPC_INST_VCTSXS>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  const std::string scaled =
      ctx.insn.operands[2] != 0
          ? std::format("simde_mm_mul_ps(simde_mm_load_ps({}.f32), simde_mm_set1_ps({}))",
                        ctx.V(ctx.insn.operands[1]), 1u << ctx.insn.operands[2])
          : std::format("simde_mm_load_ps({}.f32)", ctx.V(ctx.insn.operands[1]));
  ctx.Println("\tctx.vscr.sat |= u8(rex::ppc::simd::IsConvertSaturating<true>({}));", scaled);
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s32, rex::ppc::simd::ConvertSaturate<i32, "
      "f32>({}));",
      ctx.V(ctx.insn.operands[0]), scaled);
  return true;
}

template <>
bool Build<PPC_INST_VCFSX>(BuilderContext& ctx) {
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

template <>
bool Build<PPC_INST_VCFUX>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  ctx.Print("\tsimde_mm_store_ps({}.f32, ", ctx.V(ctx.insn.operands[0]));
  if (ctx.insn.operands[2] != 0) {
    const f32 value = std::ldexp(1.0f, -static_cast<i32>(ctx.insn.operands[2]));
    ctx.Println(
        "simde_mm_mul_ps(rex::ppc::simd::Convert<f32, "
        "u32>(simde_mm_load_si128((simde__m128i*){}.u32)"
        "), "
        "simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x{:X})))));",
        ctx.V(ctx.insn.operands[1]), std::bit_cast<u32>(value));
  } else {
    ctx.Println("rex::ppc::simd::Convert<f32, u32>(simde_mm_load_si128((simde__m128i*){}.u32)));",
                ctx.V(ctx.insn.operands[1]));
  }
  return true;
}

template <>
bool Build<PPC_INST_VCTUXS>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  const std::string scaled =
      ctx.insn.operands[2] != 0
          ? std::format("simde_mm_mul_ps(simde_mm_load_ps({}.f32), simde_mm_set1_ps({}))",
                        ctx.V(ctx.insn.operands[1]), 1u << ctx.insn.operands[2])
          : std::format("simde_mm_load_ps({}.f32)", ctx.V(ctx.insn.operands[1]));
  ctx.Println("\tctx.vscr.sat |= u8(rex::ppc::simd::IsConvertSaturating<false>({}));", scaled);
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, rex::ppc::simd::ConvertSaturate<u32, "
      "f32>({}));",
      ctx.V(ctx.insn.operands[0]), scaled);
  return true;
}

template <>
bool Build<PPC_INST_VMRGHB>(BuilderContext& ctx) {
  ctx.EmitVecIntBinarySwapped("unpackhi_epi8", "u8");
  return true;
}

template <>
bool Build<PPC_INST_VMRGHH>(BuilderContext& ctx) {
  ctx.EmitVecIntBinarySwapped("unpackhi_epi16", "u16");
  return true;
}

template <>
bool Build<PPC_INST_VMRGHW>(BuilderContext& ctx) {
  ctx.EmitVecIntBinarySwapped("unpackhi_epi32", "u32");
  return true;
}

template <>
bool Build<PPC_INST_VMRGLB>(BuilderContext& ctx) {
  ctx.EmitVecIntBinarySwapped("unpacklo_epi8", "u8");
  return true;
}

template <>
bool Build<PPC_INST_VMRGLH>(BuilderContext& ctx) {
  ctx.EmitVecIntBinarySwapped("unpacklo_epi16", "u16");
  return true;
}

template <>
bool Build<PPC_INST_VMRGLW>(BuilderContext& ctx) {
  ctx.EmitVecIntBinarySwapped("unpacklo_epi32", "u32");
  return true;
}

template <>
bool Build<PPC_INST_VPERM>(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simd::PermuteBytes(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8), simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]),
      ctx.V(ctx.insn.operands[3]));
  return true;
}

template <>
bool Build<PPC_INST_VPERMWI128>(BuilderContext& ctx) {
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

template <>
bool Build<PPC_INST_VRLIMI128>(BuilderContext& ctx) {
  constexpr size_t kShuffles[] = {SIMDE_MM_SHUFFLE(3, 2, 1, 0), SIMDE_MM_SHUFFLE(2, 1, 0, 3),
                                  SIMDE_MM_SHUFFLE(1, 0, 3, 2), SIMDE_MM_SHUFFLE(0, 3, 2, 1)};
  ctx.Println(
      "\tsimde_mm_store_ps({}.f32, simde_mm_blend_ps(simde_mm_load_ps({}.f32), "
      "simde_mm_permute_ps(simde_mm_load_ps({}.f32), {}), {}));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]),
      kShuffles[ctx.insn.operands[3]], ctx.insn.operands[2]);
  return true;
}

template <>
bool Build<PPC_INST_VSLB>(BuilderContext& ctx) {
  ctx.EmitVecVarShift("ShiftLeftLanes<u8>", "epi8", 0x7);
  return true;
}

template <>
bool Build<PPC_INST_VSLDOI>(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_alignr_epi8(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8), {}));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]),
      16 - ctx.insn.operands[3]);
  return true;
}

template <>
bool Build<PPC_INST_VSLH>(BuilderContext& ctx) {
  ctx.EmitVecVarShift("ShiftLeftLanes<u16>", "epi16", 0xF);
  return true;
}

template <>
bool Build<PPC_INST_VSRH>(BuilderContext& ctx) {
  ctx.EmitVecVarShift("ShiftRightLanes<u16>", "epi16", 0xF);
  return true;
}

template <>
bool Build<PPC_INST_VSRB>(BuilderContext& ctx) {
  for (size_t i = 0; i < 16; i++)
    ctx.Println("\t{}.u8[{}] = {}.u8[{}] >> ({}.u8[{}] & 0x7);", ctx.V(ctx.insn.operands[0]), i,
                ctx.V(ctx.insn.operands[1]), i, ctx.V(ctx.insn.operands[2]), i);
  return true;
}

template <>
bool Build<PPC_INST_VSRAB>(BuilderContext& ctx) {
  for (size_t i = 0; i < 16; i++)
    ctx.Println("\t{}.s8[{}] = {}.s8[{}] >> ({}.u8[{}] & 0x7);", ctx.V(ctx.insn.operands[0]), i,
                ctx.V(ctx.insn.operands[1]), i, ctx.V(ctx.insn.operands[2]), i);
  return true;
}

template <>
bool Build<PPC_INST_VSRAH>(BuilderContext& ctx) {
  ctx.EmitVecVarShift("ShiftRightLanes<i16>", "epi16", 0xF);
  return true;
}

template <>
bool Build<PPC_INST_VRLH>(BuilderContext& ctx) {
  auto v_d = ctx.V(ctx.insn.operands[0]);
  auto v_a = ctx.V(ctx.insn.operands[1]);
  auto v_b = ctx.V(ctx.insn.operands[2]);
  ctx.Println("\t{{");
  ctx.Println("\t\tsimde__m128i a = simde_mm_load_si128((simde__m128i*){}.u8);", v_a);
  ctx.Println("\t\tsimde__m128i sh = simde_mm_and_si128(");
  ctx.Println("\t\t\tsimde_mm_load_si128((simde__m128i*){}.u8), simde_mm_set1_epi16(0xF));", v_b);
  ctx.Println("\t\tsimde__m128i rsh = simde_mm_sub_epi16(simde_mm_set1_epi16(16), sh);");
  ctx.Println("\t\tsimde__m128i result = simde_mm_or_si128(");
  ctx.Println("\t\t\trex::ppc::simd::ShiftLeftLanes<u16>(a, sh),");
  ctx.Println("\t\t\trex::ppc::simd::ShiftRightLanes<u16>(a, rsh));");
  ctx.Println("\t\tsimde_mm_store_si128((simde__m128i*){}.u8, result);", v_d);
  ctx.Println("\t}}");
  return true;
}

template <>
bool Build<PPC_INST_VRLW>(BuilderContext& ctx) {
  for (size_t i = 0; i < 4; i++) {
    ctx.Println("\t{{ u32 sh = {}.u32[{}] & 0x1F;", ctx.V(ctx.insn.operands[2]), i);
    ctx.Println("\t{}.u32[{}] = ({}.u32[{}] << sh) | (sh ? ({}.u32[{}] >> (32 - sh)) : 0); }}",
                ctx.V(ctx.insn.operands[0]), i, ctx.V(ctx.insn.operands[1]), i,
                ctx.V(ctx.insn.operands[1]), i);
  }
  return true;
}

/** Builds vsl, which shifts the whole vector left by the bit count in the low 3 bits of vB. */
template <>
bool Build<PPC_INST_VSL>(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simd::ShiftLeftBits(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

/** Builds vslo, which shifts the whole vector left by the byte count in bits 121 to 124 of vB. */
template <>
bool Build<PPC_INST_VSLO>(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simd::ShiftLeftBytes(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

/** Builds vsro, which shifts the whole vector right by the byte count in bits 121 to 124 of vB. */
template <>
bool Build<PPC_INST_VSRO>(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simd::ShiftRightBytes(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_VSLW>(BuilderContext& ctx) {
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

template <>
bool Build<PPC_INST_VSR>(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "rex::ppc::simd::ShiftRightBits(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_load_si128((simde__m128i*){}.u8)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_VSRAW>(BuilderContext& ctx) {
  for (size_t i = 0; i < 4; i++)
    ctx.Println("\t{}.s32[{}] = {}.s32[{}] >> ({}.u8[{}] & 0x1F);", ctx.V(ctx.insn.operands[0]), i,
                ctx.V(ctx.insn.operands[1]), i, ctx.V(ctx.insn.operands[2]), i * 4);
  return true;
}

template <>
bool Build<PPC_INST_VSRW>(BuilderContext& ctx) {
  for (size_t i = 0; i < 4; i++)
    ctx.Println("\t{}.u32[{}] = {}.u32[{}] >> ({}.u8[{}] & 0x1F);", ctx.V(ctx.insn.operands[0]), i,
                ctx.V(ctx.insn.operands[1]), i, ctx.V(ctx.insn.operands[2]), i * 4);
  return true;
}

template <>
bool Build<PPC_INST_VSPLTB>(BuilderContext& ctx) {
  u32 perm = 15 - ctx.insn.operands[2];
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*){}.u8), "
      "simde_mm_set1_epi8(char(0x{:X}))));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), perm);
  return true;
}

template <>
bool Build<PPC_INST_VSPLTH>(BuilderContext& ctx) {
  u32 perm = 7 - ctx.insn.operands[2];
  perm = (perm * 2) | ((perm * 2 + 1) << 8);
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u16, "
      "simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_set1_epi16(short(0x{:X}))));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), perm);
  return true;
}

template <>
bool Build<PPC_INST_VSPLTISB>(BuilderContext& ctx) {
  i8 imm5 = static_cast<i8>(static_cast<i8>(ctx.insn.operands[1] << 3) >> 3);
  ctx.Println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_set1_epi8(char(0x{:X})));",
              ctx.V(ctx.insn.operands[0]), static_cast<u8>(imm5));
  return true;
}

template <>
bool Build<PPC_INST_VSPLTISW>(BuilderContext& ctx) {
  i8 imm5 = static_cast<i8>(static_cast<i8>(ctx.insn.operands[1] << 3) >> 3);
  ctx.Println("\tsimde_mm_store_si128((simde__m128i*){}.u32, simde_mm_set1_epi32(int(0x{:X})));",
              ctx.V(ctx.insn.operands[0]), static_cast<u32>(static_cast<i32>(imm5)));
  return true;
}

template <>
bool Build<PPC_INST_VSPLTISH>(BuilderContext& ctx) {
  i8 imm5 = static_cast<i8>(static_cast<i8>(ctx.insn.operands[1] << 3) >> 3);
  ctx.Println("\tsimde_mm_store_si128((simde__m128i*){}.s16, simde_mm_set1_epi16(short(0x{:X})));",
              ctx.V(ctx.insn.operands[0]), static_cast<u16>(static_cast<i16>(imm5)));
  return true;
}

template <>
bool Build<PPC_INST_VSPLTW>(BuilderContext& ctx) {
  u32 perm = 3 - ctx.insn.operands[2];
  perm |= (perm << 2) | (perm << 4) | (perm << 6);
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u32, "
      "simde_mm_shuffle_epi32(simde_mm_load_si128((simde__m128i*){}.u32), 0x{:X}));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), perm);
  return true;
}

template <>
bool Build<PPC_INST_VPKSHUS>(BuilderContext& ctx) {
  EmitSaturation(ctx, "IsPackSaturating<16, false>");
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_packus_epi16(simde_mm_load_si128((simde__m128i*){}.s16), "
      "simde_mm_load_si128((simde__m128i*){}.s16)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[2]), ctx.V(ctx.insn.operands[1]));
  return true;
}

/**
 * Builds vpkpx, packing the eight words of vA and vB into 1:5:5:5 pixels. Each halfword takes the
 * low bit of byte 0 and the top 5 bits of bytes 1 to 3 of its source word. vA fills vector
 * halfwords 0 to 3 and vB fills 4 to 7.
 */
template <>
bool Build<PPC_INST_VPKPX>(BuilderContext& ctx) {
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

template <>
bool Build<PPC_INST_VPKUHUM>(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u8, "
      "simde_mm_packus_epi16(simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_set1_epi16(0xFF)), simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u16), "
      "simde_mm_set1_epi16(0xFF))));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[2]), ctx.V(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_VPKUHUS>(BuilderContext& ctx) {
  EmitSaturation(ctx, "IsPackSaturating<16, false>");
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

template <>
bool Build<PPC_INST_VPKUWUM>(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u16, "
      "simde_mm_packus_epi32(simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_set1_epi32(0xFFFF)), "
      "simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){}.u32), "
      "simde_mm_set1_epi32(0xFFFF))));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[2]), ctx.V(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_VPKUWUS>(BuilderContext& ctx) {
  EmitSaturation(ctx, "IsPackSaturating<32, false>");
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

template <>
bool Build<PPC_INST_VPKSHSS>(BuilderContext& ctx) {
  EmitSaturation(ctx, "IsPackSaturating<16, true>");
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s8, "
      "simde_mm_packs_epi16(simde_mm_load_si128((simde__m128i*){}.s16), "
      "simde_mm_load_si128((simde__m128i*){}.s16)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[2]), ctx.V(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_VPKSWSS>(BuilderContext& ctx) {
  EmitSaturation(ctx, "IsPackSaturating<32, true>");
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s16, "
      "simde_mm_packs_epi32(simde_mm_load_si128((simde__m128i*){}.s32), "
      "simde_mm_load_si128((simde__m128i*){}.s32)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[2]), ctx.V(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_VPKSWUS>(BuilderContext& ctx) {
  EmitSaturation(ctx, "IsPackSaturating<32, false>");
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.u16, "
      "simde_mm_packus_epi32(simde_mm_load_si128((simde__m128i*){}.s32), "
      "simde_mm_load_si128((simde__m128i*){}.s32)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[2]), ctx.V(ctx.insn.operands[1]));
  return true;
}

/**
 * Builds vpkd3d128, packing the floats of vB into the format chosen by operand 2. Operand 3 is the
 * mask and operand 4 the shift. rex::ppc::simd::PackD3D does the conversion.
 */
template <>
bool Build<PPC_INST_VPKD3D128>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(true);
  const u32 type = ctx.insn.operands[2];
  if (type > 6) {
    ctx.Println("	__builtin_debugtrap();");
    return true;
  }
  ctx.Println("	ctx.vscr.sat |= rex::ppc::simd::PackD3D<{}, {}, {}>({}, {});", type,
              ctx.insn.operands[3], ctx.insn.operands[4], ctx.V(ctx.insn.operands[0]),
              ctx.V(ctx.insn.operands[1]));
  return true;
}

/**
 * Builds vupkd3d128, unpacking vB into floats from the format chosen by operand 2 >> 2, numbered
 * as for vpkd3d128. D3DCOLOR bytes are OR'd into the mantissa of 1.0f (0x3F800000). The SHORT and
 * NORM formats yield 0x40400000 plus the field sign extended from 16 bits (SHORT2, SHORT4), 10
 * bits (NORMPACKED32) or 20 bits (NORMPACKED64), and a field at its minimum gives the quiet NaN
 * 0x7FC00000. The NORM formats give w as 0x3F800000 OR the w bits. NORMPACKED32 reads
 * w2:z10:y10:x10 from vector element 3 (host s32[0]) and NORMPACKED64 reads w4:z20:y20:x20 from
 * vector elements 2 and 3 (host u64[0]). FLOAT16 conversion adds 0x1C000 to the 0x7C00 exponent
 * field and maps a zero exponent to a signed zero. Vector element i is host array index 3 - i. The
 * 2 and 4 element short and half float formats read the host u16 array from vector elements 2 and
 * 3, with vector element 0 taken from the highest u16 index.
 */
template <>
bool Build<PPC_INST_VUPKD3D128>(BuilderContext& ctx) {
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

template <>
bool Build<PPC_INST_VUPKHSB>(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s16, "
      "simde_mm_cvtepi8_epi16(simde_mm_unpackhi_epi64(simde_mm_load_si128((simde__m128i*){}.s8), "
      "simde_mm_load_si128((simde__m128i*){}.s8))));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_VUPKHSH>(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s32, "
      "simde_mm_cvtepi16_epi32(simde_mm_unpackhi_epi64(simde_mm_load_si128((simde__m128i*){}.s16), "
      "simde_mm_load_si128((simde__m128i*){}.s16))));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]), ctx.V(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_VUPKLSB>(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s32, "
      "simde_mm_cvtepi8_epi16(simde_mm_load_si128((simde__m128i*){}.s16)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_VUPKLSH>(BuilderContext& ctx) {
  ctx.Println(
      "\tsimde_mm_store_si128((simde__m128i*){}.s32, "
      "simde_mm_cvtepi16_epi32(simde_mm_load_si128((simde__m128i*){}.s16)));",
      ctx.V(ctx.insn.operands[0]), ctx.V(ctx.insn.operands[1]));
  return true;
}

}  // namespace rex::codegen
