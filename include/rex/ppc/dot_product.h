/**
 * @file        ppc/dot_product.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <bit>
#include <cmath>

#include <simde/x86/sse4.1.h>
#if defined(__AVX2__)
#include <simde/x86/avx2.h>
#endif

#include <rex/ppc/estimate.h>
#include <rex/types.h>

namespace rex::ppc::simd {

namespace detail {

/** The product of 2 operands as an aligned fixed point term. */
struct DotTerm {
  bool negative;
  u64 mantissa;
  i32 exponent;
};

/**
 * Splits 2 operands into the signed product term. A denormal operand is a zero, or with flush
 * clear it keeps its value as a normalized mantissa with a lower exponent.
 *
 * @param a The first operand bits.
 * @param b The second operand bits.
 * @param flush True to take a denormal operand as a zero.
 * @return The term, with a zero mantissa when either operand is zero.
 */
inline DotTerm MakeDotTerm(u32 a, u32 b, bool flush) {
  const auto split = [flush](u32 bits, u32& mantissa, i32& exponent) {
    const u32 biased = (bits >> 23) & 0xFFu;
    mantissa = biased == 0 ? 0 : ((bits & 0x7FFFFFu) | 0x800000u);
    exponent = static_cast<i32>(biased) - 127;
    if (biased == 0 && !flush && (bits & 0x7FFFFFu) != 0) {
      const i32 shift = std::countl_zero(bits & 0x7FFFFFu) - 8;
      mantissa = (bits & 0x7FFFFFu) << shift;
      exponent = -126 - shift;
    }
  };
  u32 mantissa_a;
  u32 mantissa_b;
  i32 exponent_a;
  i32 exponent_b;
  split(a, mantissa_a, exponent_a);
  split(b, mantissa_b, exponent_b);
  return {((a ^ b) & kSign32) != 0, u64{mantissa_a} * mantissa_b, exponent_a + exponent_b - 46};
}

/**
 * Normalizes the fixed point sum of a dot product to the result bits.
 *
 * @param sum The sum of the aligned terms.
 * @param majority_negative True when the majority sign is negative.
 * @param lowest The exponent of the lowest fixed point bit.
 * @return The result bits.
 */
inline u32 FinishDot(i64 sum, bool majority_negative, i32 lowest) {
  const i64 flip = sum >> 63;
  const u32 sign = (majority_negative ? kSign32 : 0u) ^ (static_cast<u32>(flip) & kSign32);
  const u64 magnitude = static_cast<u64>(sum ^ flip);
  if (magnitude == 0) {
    return 0;
  }
  const u32 width = std::bit_width(magnitude);
  const i32 exponent = lowest + static_cast<i32>(width) - 1;
  const u32 top = static_cast<u32>((magnitude << (64 - width)) >> 40);
  if (static_cast<u32>(exponent + 126) < 254u) [[likely]] {
    return sign | (static_cast<u32>(exponent + 127) << 23) | (top & 0x7FFFFFu);
  }
  if (exponent < -127) {
    return sign;
  }
  if (exponent == -127) {
    return sign | (top & 0x7FFFFFu);
  }
  if (exponent > 128) {
    return sign | kQNaN32;
  }
  return sign | (static_cast<u32>(exponent + 127) << 23) | (top & 0x7FFFFFu);
}

/**
 * Computes the dot product of the first terms of 2 operand arrays, as the 128 bit dot product
 * instructions do. The terms add as truncated fixed point values with 29 fractional bits below the
 * largest term, signed against the majority sign, and the sum is normalized by truncation.
 *
 * @tparam Terms The number of terms, 3 or 4.
 * @param a The first operand words.
 * @param b The second operand words.
 * @param flush True to flush a denormal operand to a zero, as VSCR NJ set does.
 * @return The result bits.
 */
template <u32 Terms>
inline u32 DotProductReference(const std::array<u32, Terms>& a, const std::array<u32, Terms>& b,
                               bool flush = true) {
  bool any_nan = false;
  bool any_invalid = false;
  u32 infinities = 0;
  u32 negative_infinities = 0;
  std::array<DotTerm, Terms> terms{};
  for (u32 i = 0; i < Terms; ++i) {
    const u32 x = flush && (a[i] & 0x7F800000u) == 0 ? (a[i] & kSign32) : a[i];
    const u32 y = flush && (b[i] & 0x7F800000u) == 0 ? (b[i] & kSign32) : b[i];
    const bool nan_x = (x & 0x7FFFFFFFu) > kInfinity32;
    const bool nan_y = (y & 0x7FFFFFFFu) > kInfinity32;
    const bool inf_x = (x & 0x7FFFFFFFu) == kInfinity32;
    const bool inf_y = (y & 0x7FFFFFFFu) == kInfinity32;
    const bool zero_x = (x & 0x7FFFFFFFu) == 0;
    const bool zero_y = (y & 0x7FFFFFFFu) == 0;
    any_nan |= nan_x || nan_y;
    any_invalid |= (inf_x && zero_y) || (inf_y && zero_x);
    if ((inf_x || inf_y) && !zero_x && !zero_y) {
      ++infinities;
      negative_infinities += ((x ^ y) & kSign32) != 0 ? 1 : 0;
    }
    terms[i] = MakeDotTerm(x, y, flush);
  }
  if (any_nan || any_invalid) {
    return kQNaN32;
  }
  if (infinities != 0) {
    if (negative_infinities == 0) {
      return kInfinity32;
    }
    return negative_infinities == infinities ? (kInfinity32 | kSign32) : kQNaN32;
  }
  u32 negatives = 0;
  bool any_live = false;
  i32 highest = 0;
  for (const DotTerm& term : terms) {
    negatives += term.negative ? 1 : 0;
    if (term.mantissa != 0 && (!any_live || term.exponent > highest)) {
      highest = term.exponent;
      any_live = true;
    }
  }
  if (!any_live) {
    return negatives == Terms ? kSign32 : 0u;
  }
  const bool majority_negative = 2 * negatives > Terms;
  const i32 lowest = highest + 47 - 29;
  i64 sum = 0;
  for (const DotTerm& term : terms) {
    u64 aligned = 0;
    if (term.mantissa != 0) {
      const i32 shift = lowest - term.exponent;
      aligned = shift >= 64 ? 0 : (term.mantissa >> shift);
    }
    const i64 value = static_cast<i64>(aligned);
    sum += term.negative == majority_negative ? value : ~value;
  }
  return FinishDot(sum, majority_negative, lowest);
}

/**
 * Computes the dot product of host lanes that hold no NaN or infinity, with the same result as
 * DotProductReference. The mantissa products and the alignment shifts are exact in f64.
 *
 * @tparam Terms The number of terms, 3 or 4, taken from the highest host lanes.
 * @param words_a The first operand bits.
 * @param words_b The second operand bits.
 * @param biased_a The biased exponent fields of the first operand.
 * @param biased_b The biased exponent fields of the second operand.
 * @return The result bits.
 */
template <u32 Terms>
inline u32 DotProductFinite(simde__m128i words_a, simde__m128i words_b, simde__m128i biased_a,
                            simde__m128i biased_b) {
  constexpr i32 kNone = -0x40000000;
  const auto used = simde_mm_set_epi32(-1, -1, -1, Terms == 4 ? -1 : 0);
  const auto zero = simde_mm_setzero_si128();
  const auto live_a = simde_mm_and_si128(simde_mm_cmpgt_epi32(biased_a, zero), used);
  const auto live_b = simde_mm_and_si128(simde_mm_cmpgt_epi32(biased_b, zero), used);
  const auto mantissa_a = simde_mm_and_si128(
      simde_mm_or_si128(simde_mm_and_si128(words_a, Set(0x7FFFFFu)), Set(0x800000u)), live_a);
  const auto mantissa_b = simde_mm_and_si128(
      simde_mm_or_si128(simde_mm_and_si128(words_b, Set(0x7FFFFFu)), Set(0x800000u)), live_b);
  const auto exponent = simde_mm_sub_epi32(simde_mm_add_epi32(biased_a, biased_b), Set(300));
  const auto live = simde_mm_and_si128(live_a, live_b);
  const auto candidate = Select(live, exponent, Set(static_cast<u32>(kNone)));
  auto highest_lanes = simde_mm_max_epi32(candidate, simde_mm_shuffle_epi32(candidate, 0x4E));
  highest_lanes = simde_mm_max_epi32(highest_lanes, simde_mm_shuffle_epi32(highest_lanes, 0xB1));
  const i32 highest = simde_mm_cvtsi128_si32(highest_lanes);
  const auto negative = simde_mm_srai_epi32(simde_mm_xor_si128(words_a, words_b), 31);
  const u32 negative_bits = static_cast<u32>(
      simde_mm_movemask_ps(simde_mm_castsi128_ps(simde_mm_and_si128(negative, used))));
  const u32 negatives = static_cast<u32>((0x4332322132212110ull >> (negative_bits * 4)) & 0xFu);
  if (highest == kNone) {
    return negatives == Terms ? kSign32 : 0u;
  }
  const bool majority = 2 * negatives > Terms;
  const auto minority = simde_mm_and_si128(
      simde_mm_xor_si128(negative, simde_mm_set1_epi32(-static_cast<i32>(majority))), used);
  const i32 lowest = highest + 47 - 29;
#if defined(__AVX2__)
  const auto product = simde_mm256_mul_epu32(simde_mm256_cvtepu32_epi64(mantissa_a),
                                             simde_mm256_cvtepu32_epi64(mantissa_b));
  const auto shift =
      simde_mm256_cvtepi32_epi64(simde_mm_sub_epi32(simde_mm_set1_epi32(lowest), exponent));
  const auto aligned = simde_mm256_srlv_epi64(product, shift);
  const auto terms = simde_mm256_xor_si256(aligned, simde_mm256_cvtepi32_epi64(minority));
  const auto pair = simde_mm_add_epi64(simde_mm256_castsi256_si128(terms),
                                       simde_mm256_extracti128_si256(terms, 1));
  const i64 total = simde_mm_cvtsi128_si64(pair) + simde_mm_extract_epi64(pair, 1);
  return FinishDot(total, majority, lowest);
#else
  const auto field = simde_mm_add_epi32(simde_mm_sub_epi32(exponent, simde_mm_set1_epi32(lowest)),
                                        simde_mm_set1_epi32(1023));
  const auto upper = [](simde__m128i v) { return simde_mm_shuffle_epi32(v, 0xEE); };
  const auto scale_low =
      simde_mm_castsi128_pd(simde_mm_slli_epi64(simde_mm_cvtepu32_epi64(field), 52));
  const auto scale_high =
      simde_mm_castsi128_pd(simde_mm_slli_epi64(simde_mm_cvtepu32_epi64(upper(field)), 52));
  const auto product_low =
      simde_mm_mul_pd(simde_mm_cvtepi32_pd(mantissa_a), simde_mm_cvtepi32_pd(mantissa_b));
  const auto product_high = simde_mm_mul_pd(simde_mm_cvtepi32_pd(upper(mantissa_a)),
                                            simde_mm_cvtepi32_pd(upper(mantissa_b)));
  const auto aligned =
      simde_mm_unpacklo_epi64(simde_mm_cvttpd_epi32(simde_mm_mul_pd(product_low, scale_low)),
                              simde_mm_cvttpd_epi32(simde_mm_mul_pd(product_high, scale_high)));
  const auto terms = simde_mm_xor_si128(aligned, minority);
  const auto pair =
      simde_mm_add_epi64(simde_mm_cvtepi32_epi64(terms), simde_mm_cvtepi32_epi64(upper(terms)));
  const i64 total = simde_mm_cvtsi128_si64(pair) + simde_mm_extract_epi64(pair, 1);
  return FinishDot(total, majority, lowest);
#endif
}

/**
 * Computes the dot product of the first elements of 2 vectors and broadcasts it to all lanes.
 *
 * @tparam Terms The number of elements, 3 or 4.
 * @param a The first operand.
 * @param b The second operand.
 * @param flush True to flush a denormal operand to a zero, as VSCR NJ set does.
 * @return The result in all 4 lanes.
 */
template <u32 Terms>
inline simde__m128 DotProductVector(simde__m128 a, simde__m128 b, bool flush) {
  const auto words_a = simde_mm_castps_si128(a);
  const auto words_b = simde_mm_castps_si128(b);
  const auto used = simde_mm_set_epi32(-1, -1, -1, Terms == 4 ? -1 : 0);
  const auto biased_a = simde_mm_srli_epi32(simde_mm_and_si128(words_a, Set(kInfinity32)), 23);
  const auto biased_b = simde_mm_srli_epi32(simde_mm_and_si128(words_b, Set(kInfinity32)), 23);
  const auto infinite = simde_mm_set1_epi32(0xFF);
  auto special = simde_mm_or_si128(simde_mm_cmpeq_epi32(biased_a, infinite),
                                   simde_mm_cmpeq_epi32(biased_b, infinite));
  if (!flush) [[unlikely]] {
    const auto zero = simde_mm_setzero_si128();
    const auto denormal_a = simde_mm_andnot_si128(
        simde_mm_cmpeq_epi32(simde_mm_and_si128(words_a, Set(0x7FFFFFu)), zero),
        Equal(simde_mm_and_si128(words_a, Set(kInfinity32)), 0));
    const auto denormal_b = simde_mm_andnot_si128(
        simde_mm_cmpeq_epi32(simde_mm_and_si128(words_b, Set(0x7FFFFFu)), zero),
        Equal(simde_mm_and_si128(words_b, Set(kInfinity32)), 0));
    special = simde_mm_or_si128(special, simde_mm_or_si128(denormal_a, denormal_b));
  }
  special = simde_mm_and_si128(special, used);
  u32 result;
  if (simde_mm_movemask_epi8(special) == 0) {
    result = DotProductFinite<Terms>(words_a, words_b, biased_a, biased_b);
  } else {
    alignas(16) std::array<u32, 4> host_a;
    alignas(16) std::array<u32, 4> host_b;
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(host_a.data()), words_a);
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(host_b.data()), words_b);
    std::array<u32, Terms> terms_a;
    std::array<u32, Terms> terms_b;
    for (u32 i = 0; i < Terms; ++i) {
      terms_a[i] = host_a[3 - i];
      terms_b[i] = host_b[3 - i];
    }
    result = DotProductReference<Terms>(terms_a, terms_b, flush);
  }
  return simde_mm_castsi128_ps(
      simde_mm_set1_epi32(static_cast<i32>(DotProduct<Terms>(terms_a, terms_b))));
}

}  // namespace detail

/**
 * Computes vmsum3fp128, the dot product of the first 3 vector elements.
 *
 * @param a The first operand.
 * @param b The second operand.
 * @return The dot product in all 4 lanes.
 */
[[nodiscard]] inline simde__m128 DotProduct3(simde__m128 a, simde__m128 b) noexcept {
  return detail::DotProductVector<3>(a, b, true);
}

/**
 * Computes vmsum3fp128, the dot product of the first 3 elements, honoring VSCR NJ.
 *
 * @param a The first operand.
 * @param b The second operand.
 * @param non_java The VSCR NJ bit. When clear a denormal operand keeps its value.
 * @return The dot product in all 4 lanes.
 */
[[nodiscard]] inline simde__m128 DotProduct3(simde__m128 a, simde__m128 b, bool non_java) noexcept {
  return detail::DotProductVector<3>(a, b, non_java);
}

/**
 * Computes vmsum4fp128, the dot product of all 4 vector elements.
 *
 * @param a The first operand.
 * @param b The second operand.
 * @return The dot product in all 4 lanes.
 */
[[nodiscard]] inline simde__m128 DotProduct4(simde__m128 a, simde__m128 b) noexcept {
  return detail::DotProductVector<4>(a, b, true);
}

/**
 * Computes vmsum4fp128, the dot product of all 4 elements, honoring VSCR NJ.
 *
 * @param a The first operand.
 * @param b The second operand.
 * @param non_java The VSCR NJ bit. When clear a denormal operand keeps its value.
 * @return The dot product in all 4 lanes.
 */
[[nodiscard]] inline simde__m128 DotProduct4(simde__m128 a, simde__m128 b, bool non_java) noexcept {
  return detail::DotProductVector<4>(a, b, non_java);
}

}  // namespace rex::ppc::simd
