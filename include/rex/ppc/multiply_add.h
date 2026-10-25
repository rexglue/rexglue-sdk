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

#include <cmath>

#include <simde/x86/fma.h>
#include <simde/x86/sse4.1.h>

#include <rex/ppc/estimate.h>
#include <rex/types.h>

namespace rex::ppc::simd {

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
 * @param flush True to flush a result below the smallest f32 normal to a zero.
 * @return The 2 rounded f32 results in the low half.
 */
inline simde__m128 MultiplyAddHalf(simde__m128 a, simde__m128 b, simde__m128 c, bool flush) {
  const auto product = simde_mm_mul_pd(simde_mm_cvtps_pd(a), simde_mm_cvtps_pd(b));
  const auto sum = SumToOdd(product, simde_mm_cvtps_pd(c));
  return simde_mm_cvtpd_ps(flush ? FlushTiny(sum) : sum);
}

/**
 * Computes the fused multiply add of 4 lanes, or its negated subtract form, for any operands.
 *
 * @tparam Negate True for -(a * b - c).
 * @param a The first multiplier.
 * @param b The second multiplier.
 * @param c The addend.
 * @param flush True for VSCR NJ set, which flushes denormal operands and results to a zero.
 * @return The results.
 */
template <bool Negate>
inline simde__m128 MultiplyAddFull(simde__m128 a, simde__m128 b, simde__m128 c, bool flush) {
  const auto bits_a = simde_mm_castps_si128(a);
  const auto bits_b = simde_mm_castps_si128(b);
  const auto bits_c = simde_mm_castps_si128(c);
  const auto flushed_a = flush ? simde_mm_castsi128_ps(FlushDenormals(bits_a)) : a;
  const auto flushed_b = flush ? simde_mm_castsi128_ps(FlushDenormals(bits_b)) : b;
  auto addend = flush ? FlushDenormals(bits_c) : bits_c;
  if constexpr (Negate) {
    addend = simde_mm_xor_si128(addend, Set(kSign32));
  }
  const auto addend_f = simde_mm_castsi128_ps(addend);
  const auto low = MultiplyAddHalf(flushed_a, flushed_b, addend_f, flush);
  const auto high = MultiplyAddHalf(simde_mm_movehl_ps(flushed_a, flushed_a),
                                    simde_mm_movehl_ps(flushed_b, flushed_b),
                                    simde_mm_movehl_ps(addend_f, addend_f), flush);
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

/**
 * Computes the fused multiply add of 4 lanes, or its negated subtract form, with the f64 sums. With
 * VSCR NJ set and no NaN or infinity lane, a lane whose product and addend are close enough in
 * magnitude sums exactly in f64, so the rounding to odd is skipped. Every other case takes
 * MultiplyAddFull.
 *
 * @tparam Negate True for -(a * b - c).
 * @param a The first multiplier.
 * @param b The second multiplier.
 * @param c The addend.
 * @param flush True for VSCR NJ set, which flushes denormal operands and results to a zero.
 * @return The results.
 */
template <bool Negate>
inline simde__m128 MultiplyAddWide(simde__m128 a, simde__m128 b, simde__m128 c, bool flush) {
  if (!flush) [[unlikely]] {
    return MultiplyAddFull<Negate>(a, b, c, flush);
  }
  const auto flushed_a = simde_mm_castsi128_ps(FlushDenormals(simde_mm_castps_si128(a)));
  const auto flushed_b = simde_mm_castsi128_ps(FlushDenormals(simde_mm_castps_si128(b)));
  const auto flushed_c = simde_mm_castsi128_ps(FlushDenormals(simde_mm_castps_si128(c)));
  const auto exponent_mask = Set(kInfinity32);
  const auto biased_a =
      simde_mm_srli_epi32(simde_mm_and_si128(simde_mm_castps_si128(flushed_a), exponent_mask), 23);
  const auto biased_b =
      simde_mm_srli_epi32(simde_mm_and_si128(simde_mm_castps_si128(flushed_b), exponent_mask), 23);
  const auto biased_c =
      simde_mm_srli_epi32(simde_mm_and_si128(simde_mm_castps_si128(flushed_c), exponent_mask), 23);
  const auto all_ones = simde_mm_set1_epi32(0xFF);
  const auto special =
      simde_mm_or_si128(simde_mm_or_si128(simde_mm_cmpeq_epi32(biased_a, all_ones),
                                          simde_mm_cmpeq_epi32(biased_b, all_ones)),
                        simde_mm_cmpeq_epi32(biased_c, all_ones));
  const auto zero = simde_mm_setzero_si128();
  const auto any_zero = simde_mm_or_si128(
      simde_mm_or_si128(simde_mm_cmpeq_epi32(biased_a, zero), simde_mm_cmpeq_epi32(biased_b, zero)),
      simde_mm_cmpeq_epi32(biased_c, zero));
  const auto gap = simde_mm_sub_epi32(simde_mm_add_epi32(biased_a, biased_b),
                                      simde_mm_add_epi32(biased_c, simde_mm_set1_epi32(127)));
  const auto close = simde_mm_and_si128(simde_mm_cmpgt_epi32(gap, simde_mm_set1_epi32(-5)),
                                        simde_mm_cmpgt_epi32(simde_mm_set1_epi32(28), gap));
  const auto exact = simde_mm_or_si128(any_zero, close);
  if (simde_mm_movemask_epi8(simde_mm_or_si128(
          special, simde_mm_andnot_si128(exact, simde_mm_cmpeq_epi32(zero, zero)))) != 0)
      [[unlikely]] {
    return MultiplyAddFull<Negate>(a, b, c, flush);
  }
  auto addend = simde_mm_castps_si128(flushed_c);
  if constexpr (Negate) {
    addend = simde_mm_xor_si128(addend, Set(kSign32));
  }
  const auto addend_f = simde_mm_castsi128_ps(addend);
  const auto sum_low =
      simde_mm_add_pd(simde_mm_mul_pd(simde_mm_cvtps_pd(flushed_a), simde_mm_cvtps_pd(flushed_b)),
                      simde_mm_cvtps_pd(addend_f));
  const auto sum_high =
      simde_mm_add_pd(simde_mm_mul_pd(simde_mm_cvtps_pd(simde_mm_movehl_ps(flushed_a, flushed_a)),
                                      simde_mm_cvtps_pd(simde_mm_movehl_ps(flushed_b, flushed_b))),
                      simde_mm_cvtps_pd(simde_mm_movehl_ps(addend_f, addend_f)));
  auto result = simde_mm_castps_si128(simde_mm_movelh_ps(simde_mm_cvtpd_ps(FlushTiny(sum_low)),
                                                         simde_mm_cvtpd_ps(FlushTiny(sum_high))));
  if constexpr (Negate) {
    result = simde_mm_xor_si128(result, Set(kSign32));
  }
  return simde_mm_castsi128_ps(result);
}

#if defined(__FMA__)
/**
 * Computes the fused multiply add of 4 lanes, or its negated subtract form, with the host fused
 * instruction. The operands are flushed first, so the host denormals are zero mode does not
 * matter, and a tiny result is flushed afterwards, so the host flush to zero mode does not matter
 * either. A NaN result lane, which covers a NaN operand and an invalid product or sum, and a result
 * of exactly the smallest normal go to MultiplyAddFull. The host detects a tiny result after
 * rounding and the guest before it, so only that magnitude can differ between the two.
 *
 * @tparam Negate True for -(a * b - c).
 * @param a The first multiplier.
 * @param b The second multiplier.
 * @param c The addend.
 * @return The results.
 */
template <bool Negate>
inline simde__m128 MultiplyAddFused(simde__m128 a, simde__m128 b, simde__m128 c) {
  const auto flushed_a = simde_mm_castsi128_ps(FlushDenormals(simde_mm_castps_si128(a)));
  const auto flushed_b = simde_mm_castsi128_ps(FlushDenormals(simde_mm_castps_si128(b)));
  const auto flushed_c = simde_mm_castsi128_ps(FlushDenormals(simde_mm_castps_si128(c)));
  const auto raw = Negate ? simde_mm_fmsub_ps(flushed_a, flushed_b, flushed_c)
                          : simde_mm_fmadd_ps(flushed_a, flushed_b, flushed_c);
  const auto bits = simde_mm_castps_si128(raw);
  const auto magnitude = simde_mm_andnot_si128(Set(kSign32), bits);
  const auto unordered = simde_mm_castps_si128(simde_mm_cmpunord_ps(raw, raw));
  const auto smallest = simde_mm_cmpeq_epi32(magnitude, Set(0x00800000u));
  if (simde_mm_movemask_epi8(simde_mm_or_si128(unordered, smallest)) != 0) [[unlikely]] {
    return MultiplyAddFull<Negate>(a, b, c, true);
  }
  const auto tiny = simde_mm_cmpgt_epi32(Set(0x00800000u), magnitude);
  auto result = Select(tiny, simde_mm_and_si128(bits, Set(kSign32)), bits);
  if constexpr (Negate) {
    result = simde_mm_xor_si128(result, Set(kSign32));
  }
  return simde_mm_castsi128_ps(result);
}
#endif

/**
 * Computes the fused multiply add of 4 lanes, or its negated subtract form, for any operands.
 *
 * @tparam Negate True for -(a * b - c).
 * @param a The first multiplier.
 * @param b The second multiplier.
 * @param c The addend.
 * @param flush True for VSCR NJ set, which flushes denormal operands and results to a zero.
 * @return The results.
 */
template <bool Negate>
inline simde__m128 MultiplyAdd(simde__m128 a, simde__m128 b, simde__m128 c, bool flush) {
#if defined(__FMA__)
  if (flush) [[likely]] {
    return MultiplyAddFused<Negate>(a, b, c);
  }
  return MultiplyAddFull<Negate>(a, b, c, flush);
#else
  return MultiplyAddWide<Negate>(a, b, c, flush);
#endif
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
[[nodiscard]] inline simde__m128 MultiplyAdd(simde__m128 a, simde__m128 b, simde__m128 c) noexcept {
  return detail::MultiplyAdd<false>(a, b, c, true);
}

/**
 * Computes vmaddfp, vmaddfp128 and vmaddcfp128 with a single rounding, honoring VSCR NJ.
 *
 * @param a The first multiplier.
 * @param b The second multiplier.
 * @param c The addend.
 * @param non_java The VSCR NJ bit. When clear a denormal operand and a denormal result are kept.
 * @return a * b + c in each lane.
 */
[[nodiscard]] inline simde__m128 MultiplyAdd(simde__m128 a, simde__m128 b, simde__m128 c,
                                             bool non_java) noexcept {
  return detail::MultiplyAdd<false>(a, b, c, non_java);
}

/**
 * Computes vnmsubfp and vnmsubfp128 with a single rounding.
 *
 * @param a The first multiplier.
 * @param b The second multiplier.
 * @param c The value subtracted from the product.
 * @return -(a * b - c) in each lane.
 */
[[nodiscard]] inline simde__m128 NegativeMultiplySubtract(simde__m128 a, simde__m128 b,
                                                          simde__m128 c) noexcept {
  return detail::MultiplyAdd<true>(a, b, c, true);
}

/**
 * Computes vnmsubfp and vnmsubfp128 with a single rounding, honoring VSCR NJ.
 *
 * @param a The first multiplier.
 * @param b The second multiplier.
 * @param c The value subtracted from the product.
 * @param non_java The VSCR NJ bit. When clear a denormal operand and a denormal result are kept.
 * @return -(a * b - c) in each lane.
 */
[[nodiscard]] inline simde__m128 NegativeMultiplySubtract(simde__m128 a, simde__m128 b,
                                                          simde__m128 c, bool non_java) noexcept {
  return detail::MultiplyAdd<true>(a, b, c, non_java);
}

}  // namespace rex::ppc::simd
