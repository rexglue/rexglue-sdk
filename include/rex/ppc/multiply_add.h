/**
 * @file        ppc/multiply_add.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <simde/x86/sse4.1.h>

#include <rex/ppc/estimate.h>
#include <rex/types.h>

namespace rex::ppc {

namespace detail {

/**
 * Replaces each denormal lane with a zero of the same sign.
 *
 * @param v The lanes.
 * @return The lanes with the denormals flushed.
 */
inline simde__m128i FlushDenormals(simde__m128i v) {
  const auto denormal = Equal(simde_mm_and_si128(v, Set(kInfinity32)), 0);
  return Select(denormal, simde_mm_and_si128(v, Set(kSign32)), v);
}

/**
 * Adds 2 f64 lanes and rounds the sum to odd, so that a later narrowing rounds once.
 *
 * @param p The first addend.
 * @param c The second addend.
 * @return The sum with an inexact result forced to an odd last bit.
 */
inline simde__m128d SumToOdd(simde__m128d p, simde__m128d c) {
  const auto s = simde_mm_add_pd(p, c);
  const auto virtual_c = simde_mm_sub_pd(s, p);
  const auto err = simde_mm_add_pd(simde_mm_sub_pd(p, simde_mm_sub_pd(s, virtual_c)),
                                   simde_mm_sub_pd(c, virtual_c));
  const auto inexact =
      simde_mm_and_pd(simde_mm_cmpord_pd(err, err), simde_mm_cmpneq_pd(err, simde_mm_setzero_pd()));
  const auto bits = simde_mm_castpd_si128(s);
  const auto even = simde_mm_cmpeq_epi64(simde_mm_and_si128(bits, simde_mm_set1_epi64x(1)),
                                         simde_mm_setzero_si128());
  const auto differ = simde_mm_srli_epi64(simde_mm_xor_si128(bits, simde_mm_castpd_si128(err)), 63);
  const auto step = simde_mm_sub_epi64(simde_mm_set1_epi64x(1), simde_mm_slli_epi64(differ, 1));
  const auto adjust =
      simde_mm_and_si128(simde_mm_castpd_si128(inexact), simde_mm_and_si128(even, step));
  return simde_mm_castsi128_pd(simde_mm_add_epi64(bits, adjust));
}

/**
 * Replaces each f64 lane below the smallest f32 normal with a zero of the same sign.
 *
 * @param s The lanes.
 * @return The lanes with the tiny magnitudes flushed.
 */
inline simde__m128d FlushTiny(simde__m128d s) {
  const auto sign = simde_mm_castsi128_pd(simde_mm_set1_epi64x(static_cast<i64>(kSign64)));
  const auto tiny = simde_mm_cmplt_pd(simde_mm_andnot_pd(sign, s), simde_mm_set1_pd(0x1p-126));
  return simde_mm_blendv_pd(s, simde_mm_and_pd(s, sign), tiny);
}

/**
 * Computes a * b + c with a single rounding on the 2 low lanes.
 *
 * @param a The first multiplier lanes.
 * @param b The second multiplier lanes.
 * @param c The addend lanes.
 * @return The 2 rounded f32 results in the low half.
 */
inline simde__m128 MultiplyAddHalf(simde__m128 a, simde__m128 b, simde__m128 c) {
  const auto product = simde_mm_mul_pd(simde_mm_cvtps_pd(a), simde_mm_cvtps_pd(b));
  return simde_mm_cvtpd_ps(FlushTiny(SumToOdd(product, simde_mm_cvtps_pd(c))));
}

/**
 * Computes the fused multiply add of 4 lanes, or its negated subtract form.
 *
 * @tparam Negate True for -(a * b - c).
 * @param a The first multiplier.
 * @param b The second multiplier.
 * @param c The addend.
 * @return The results.
 */
template <bool Negate>
inline simde__m128 MultiplyAdd(simde__m128 a, simde__m128 b, simde__m128 c) {
  const auto bits_a = simde_mm_castps_si128(a);
  const auto bits_b = simde_mm_castps_si128(b);
  const auto bits_c = simde_mm_castps_si128(c);
  const auto flushed_a = simde_mm_castsi128_ps(FlushDenormals(bits_a));
  const auto flushed_b = simde_mm_castsi128_ps(FlushDenormals(bits_b));
  auto addend = FlushDenormals(bits_c);
  if constexpr (Negate) {
    addend = simde_mm_xor_si128(addend, Set(kSign32));
  }
  const auto addend_f = simde_mm_castsi128_ps(addend);
  const auto low = MultiplyAddHalf(flushed_a, flushed_b, addend_f);
  const auto high = MultiplyAddHalf(simde_mm_movehl_ps(flushed_a, flushed_a),
                                    simde_mm_movehl_ps(flushed_b, flushed_b),
                                    simde_mm_movehl_ps(addend_f, addend_f));
  auto result = simde_mm_castps_si128(simde_mm_movelh_ps(low, high));
  if constexpr (Negate) {
    result = simde_mm_xor_si128(result, Set(kSign32));
  }
  const auto invalid = simde_mm_castps_si128(
      simde_mm_cmpunord_ps(simde_mm_castsi128_ps(result), simde_mm_castsi128_ps(result)));
  result = Select(invalid, Set(kQNaN32), result);
  for (const auto operand : {bits_b, bits_c, bits_a}) {
    const auto nan = simde_mm_castps_si128(
        simde_mm_cmpunord_ps(simde_mm_castsi128_ps(operand), simde_mm_castsi128_ps(operand)));
    result = Select(nan, simde_mm_or_si128(operand, Set(kQuiet32)), result);
  }
  return simde_mm_castsi128_ps(result);
}

}  // namespace detail

/**
 * Computes vmaddfp, vmaddfp128 and vmaddcfp128 with a single rounding.
 *
 * @param a The first multiplier.
 * @param b The second multiplier.
 * @param c The addend.
 * @return a * b + c in each lane.
 */
[[nodiscard]] inline simde__m128 Vmaddfp(simde__m128 a, simde__m128 b, simde__m128 c) noexcept {
  return detail::MultiplyAdd<false>(a, b, c);
}

/**
 * Computes vnmsubfp and vnmsubfp128 with a single rounding.
 *
 * @param a The first multiplier.
 * @param b The second multiplier.
 * @param c The value subtracted from the product.
 * @return -(a * b - c) in each lane.
 */
[[nodiscard]] inline simde__m128 Vnmsubfp(simde__m128 a, simde__m128 b, simde__m128 c) noexcept {
  return detail::MultiplyAdd<true>(a, b, c);
}

}  // namespace rex::ppc
