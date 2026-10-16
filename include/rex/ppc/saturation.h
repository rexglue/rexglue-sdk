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

#include <array>
#include <cmath>

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

namespace detail {

/** The integer range a packed field accepts, with a flag for fields that never saturate. */
struct PackRange {
  bool checked;
  f64 low;
  f64 high;
};

inline constexpr PackRange kUnchecked{false, 0.0, 0.0};
inline constexpr PackRange kByteRange{true, 0.0, 255.0};
inline constexpr PackRange kShortRange{true, -32767.0, 32767.0};
inline constexpr PackRange kSigned10Range{true, -511.0, 511.0};
inline constexpr PackRange kSigned20Range{true, -524287.0, 524287.0};
inline constexpr PackRange kTwoBitRange{true, 0.0, 3.0};
inline constexpr PackRange kFourBitRange{true, 0.0, 15.0};

/** The range of each vector element for the pack format with the index of the table entry. */
inline constexpr std::array<std::array<PackRange, 4>, 7> kPackD3DRanges = {{
    {kByteRange, kByteRange, kByteRange, kByteRange},
    {kShortRange, kShortRange, kUnchecked, kUnchecked},
    {kSigned10Range, kSigned10Range, kSigned10Range, kTwoBitRange},
    {kUnchecked, kUnchecked, kUnchecked, kUnchecked},
    {kShortRange, kShortRange, kShortRange, kShortRange},
    {kUnchecked, kUnchecked, kUnchecked, kUnchecked},
    {kSigned20Range, kSigned20Range, kSigned20Range, kFourBitRange},
}};

/** The lowest or highest float of the range of each host lane, or 0 for a lane that never
 * saturates. */
template <u32 Type, bool High>
inline constexpr std::array<f32, 4> kPackD3DBounds = [] {
  std::array<f32, 4> bounds{};
  for (u32 lane = 0; lane < 4; ++lane) {
    const PackRange& range = kPackD3DRanges[Type][3 - lane];
    bounds[lane] =
        range.checked ? static_cast<f32>(3.0 + (High ? range.high : range.low) / 4194304.0) : 0.0f;
  }
  return bounds;
}();

/** The all ones mask of each host lane that can saturate. */
template <u32 Type>
inline constexpr std::array<u32, 4> kPackD3DChecked = [] {
  std::array<u32, 4> mask{};
  for (u32 lane = 0; lane < 4; ++lane) {
    mask[lane] = kPackD3DRanges[Type][3 - lane].checked ? 0xFFFFFFFFu : 0u;
  }
  return mask;
}();

}  // namespace detail

/**
 * Reports whether a vpkd3d128 pack saturates. Every vector element counts, whatever the mask and
 * shift write. A NaN element never saturates. Floats in [2, 4) are spaced by 2^-22, so each one
 * is 3.0 plus an integer multiple of 2^-22, and every other finite float lies beyond every range.
 *
 * @tparam Type The pack format, 0 to 6.
 * @param source The vector being packed.
 * @return True when an element lies outside the range of its packed field.
 */
template <u32 Type>
[[nodiscard]] inline bool PackD3DSaturates(simde__m128 source) noexcept {
  const auto low = simde_mm_loadu_ps(detail::kPackD3DBounds<Type, false>.data());
  const auto high = simde_mm_loadu_ps(detail::kPackD3DBounds<Type, true>.data());
  const auto checked =
      simde_mm_loadu_ps(reinterpret_cast<const f32*>(detail::kPackD3DChecked<Type>.data()));
  const auto inside =
      simde_mm_and_ps(simde_mm_cmpge_ps(source, low), simde_mm_cmple_ps(source, high));
  const auto outside = simde_mm_andnot_ps(inside, simde_mm_cmpord_ps(source, source));
  return simde_mm_movemask_ps(simde_mm_and_ps(outside, checked)) != 0;
}

}  // namespace rex::ppc
