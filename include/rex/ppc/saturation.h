/**
 * @file        ppc/saturation.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <simde/x86/sse4.1.h>

#include <rex/types.h>

namespace rex::ppc {

/**
 * Reports whether a float to fixed point conversion clamps any lane. A NaN lane gives 0 without a
 * clamp, and an infinity or an out of range value clamps.
 *
 * @tparam Signed True for the signed word conversion, false for the unsigned one.
 * @param scaled The floats after the scale multiply.
 * @return True when a lane lies outside the range of the destination word.
 */
template <bool Signed>
[[nodiscard]] inline bool ConvertSaturates(simde__m128 scaled) noexcept {
  const auto above = simde_mm_cmpge_ps(scaled, simde_mm_set1_ps(Signed ? 0x1p31f : 0x1p32f));
  const auto below = Signed ? simde_mm_cmplt_ps(scaled, simde_mm_set1_ps(-0x1p31f))
                            : simde_mm_cmple_ps(scaled, simde_mm_set1_ps(-1.0f));
  return simde_mm_movemask_ps(simde_mm_or_ps(above, below)) != 0;
}

namespace detail {

/**
 * Reports whether a saturating result differs from the wrapped one in any lane.
 *
 * @param wrapped The result with modulo arithmetic.
 * @param saturated The result with the clamped arithmetic.
 * @return True when at least 1 lane differs.
 */
inline bool AnyLaneDiffers(simde__m128i wrapped, simde__m128i saturated) {
  return simde_mm_movemask_epi8(simde_mm_cmpeq_epi8(wrapped, saturated)) != 0xFFFF;
}

}  // namespace detail

/**
 * Reports whether a saturating vector add clamps any lane.
 *
 * @tparam Bits The element width, 8, 16 or 32.
 * @tparam Signed True for the signed forms, false for the unsigned ones.
 * @param a The first operand.
 * @param b The second operand.
 * @return True when a lane overflows its element range.
 */
template <u32 Bits, bool Signed>
[[nodiscard]] inline bool AddSaturates(simde__m128i a, simde__m128i b) noexcept {
  if constexpr (Bits == 8) {
    return detail::AnyLaneDiffers(simde_mm_add_epi8(a, b),
                                  Signed ? simde_mm_adds_epi8(a, b) : simde_mm_adds_epu8(a, b));
  } else if constexpr (Bits == 16) {
    return detail::AnyLaneDiffers(simde_mm_add_epi16(a, b),
                                  Signed ? simde_mm_adds_epi16(a, b) : simde_mm_adds_epu16(a, b));
  } else if constexpr (Signed) {
    const auto sum = simde_mm_add_epi32(a, b);
    const auto overflow =
        simde_mm_and_si128(simde_mm_xor_si128(a, sum), simde_mm_xor_si128(b, sum));
    return simde_mm_movemask_ps(simde_mm_castsi128_ps(overflow)) != 0;
  } else {
    const auto sum = simde_mm_add_epi32(a, b);
    return simde_mm_movemask_epi8(simde_mm_cmpeq_epi32(simde_mm_max_epu32(a, sum), sum)) != 0xFFFF;
  }
}

/**
 * Reports whether a saturating vector subtract clamps any lane.
 *
 * @tparam Bits The element width, 8, 16 or 32.
 * @tparam Signed True for the signed forms, false for the unsigned ones.
 * @param a The minuend.
 * @param b The subtrahend.
 * @return True when a lane overflows its element range.
 */
template <u32 Bits, bool Signed>
[[nodiscard]] inline bool SubSaturates(simde__m128i a, simde__m128i b) noexcept {
  if constexpr (Bits == 8) {
    return detail::AnyLaneDiffers(simde_mm_sub_epi8(a, b),
                                  Signed ? simde_mm_subs_epi8(a, b) : simde_mm_subs_epu8(a, b));
  } else if constexpr (Bits == 16) {
    return detail::AnyLaneDiffers(simde_mm_sub_epi16(a, b),
                                  Signed ? simde_mm_subs_epi16(a, b) : simde_mm_subs_epu16(a, b));
  } else if constexpr (Signed) {
    const auto difference = simde_mm_sub_epi32(a, b);
    const auto overflow =
        simde_mm_and_si128(simde_mm_xor_si128(a, b), simde_mm_xor_si128(a, difference));
    return simde_mm_movemask_ps(simde_mm_castsi128_ps(overflow)) != 0;
  } else {
    return simde_mm_movemask_epi8(simde_mm_cmpeq_epi32(simde_mm_max_epu32(a, b), a)) != 0xFFFF;
  }
}

/**
 * Reports whether a saturating vector pack clamps any lane of its 2 operands.
 *
 * @tparam SourceBits The source element width, 16 or 32.
 * @tparam SignedDest True when the packed elements are signed.
 * @param a The first source operand.
 * @param b The second source operand.
 * @return True when a source element lies outside the range of the packed element.
 */
template <u32 SourceBits, bool SignedDest>
[[nodiscard]] inline bool PackSaturates(simde__m128i a, simde__m128i b) noexcept {
  constexpr u32 kDestBits = SourceBits / 2;
  constexpr u32 kRange = (1u << kDestBits) - 1;
  constexpr u32 kLow = SignedDest ? (0u - (1u << (kDestBits - 1))) : 0u;
  if constexpr (SourceBits == 16) {
    const auto low = simde_mm_set1_epi16(static_cast<i16>(kLow));
    const auto range = simde_mm_set1_epi16(static_cast<i16>(kRange));
    const auto outside = simde_mm_or_si128(simde_mm_subs_epu16(simde_mm_sub_epi16(a, low), range),
                                           simde_mm_subs_epu16(simde_mm_sub_epi16(b, low), range));
    return simde_mm_movemask_epi8(simde_mm_cmpeq_epi8(outside, simde_mm_setzero_si128())) != 0xFFFF;
  } else {
    const auto low = simde_mm_set1_epi32(static_cast<i32>(kLow));
    const auto range = simde_mm_set1_epi32(static_cast<i32>(kRange));
    const auto first = simde_mm_sub_epi32(a, low);
    const auto second = simde_mm_sub_epi32(b, low);
    const auto inside =
        simde_mm_and_si128(simde_mm_cmpeq_epi32(simde_mm_max_epu32(first, range), range),
                           simde_mm_cmpeq_epi32(simde_mm_max_epu32(second, range), range));
    return simde_mm_movemask_epi8(inside) != 0xFFFF;
  }
}

}  // namespace rex::ppc
