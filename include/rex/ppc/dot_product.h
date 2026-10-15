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

#include <simde/x86/sse4.1.h>

#include <rex/ppc/estimate.h>
#include <rex/types.h>

namespace rex::ppc {

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
  bool any_na_n = false;
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
    any_na_n |= nan_x || nan_y;
    any_invalid |= (inf_x && zero_y) || (inf_y && zero_x);
    if ((inf_x || inf_y) && !zero_x && !zero_y) {
      ++infinities;
      negative_infinities += ((x ^ y) & kSign32) != 0 ? 1 : 0;
    }
    terms[i] = MakeDotTerm(x, y, flush);
  }
  if (any_na_n || any_invalid) {
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
  bool negative = majority_negative;
  if (sum < 0) {
    negative = !majority_negative;
    sum = ~sum;
  }
  if (sum == 0) {
    return 0;
  }
  const u32 width = std::bit_width(static_cast<u64>(sum));
  const i32 exponent = lowest + static_cast<i32>(width) - 1;
  const u32 sign = negative ? kSign32 : 0u;
  const u64 magnitude = static_cast<u64>(sum);
  const u32 top =
      static_cast<u32>(width > 24 ? magnitude >> (width - 24) : magnitude << (24 - width));
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
  auto special =
      simde_mm_or_si128(Equal(simde_mm_and_si128(words_a, Set(kInfinity32)), kInfinity32),
                        Equal(simde_mm_and_si128(words_b, Set(kInfinity32)), kInfinity32));
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
    result = DotProductFinite<Terms>(words_a, words_b);
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
[[nodiscard]] inline simde__m128 Vmsum3fp128(simde__m128 a, simde__m128 b) noexcept {
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
[[nodiscard]] inline simde__m128 Vmsum3fp128(simde__m128 a, simde__m128 b, bool non_java) noexcept {
  return detail::DotProductVector<3>(a, b, non_java);
}

/**
 * Computes vmsum4fp128, the dot product of all 4 vector elements.
 *
 * @param a The first operand.
 * @param b The second operand.
 * @return The dot product in all 4 lanes.
 */
[[nodiscard]] inline simde__m128 Vmsum4fp128(simde__m128 a, simde__m128 b) noexcept {
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
[[nodiscard]] inline simde__m128 Vmsum4fp128(simde__m128 a, simde__m128 b, bool non_java) noexcept {
  return detail::DotProductVector<4>(a, b, non_java);
}

}  // namespace rex::ppc
