/**
 * @file        ppc/pack_d3d.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>

#include <rex/ppc/context.h>
#include <rex/ppc/saturation.h>
#include <rex/types.h>

namespace rex::ppc::simd {

namespace detail {

/** The placement and NaN field of a vector element in the 64 bit packed result. */
struct PackField {
  u8 shift;
  u8 width;
  u32 nan_field;
};

/** The lanes of a pack format in element order, with the number of lanes the format packs. */
struct PackLayout {
  std::array<PackField, 4> lanes;
  u32 count;
  bool half;
};

inline constexpr std::array<PackLayout, 7> kPackLayouts = {{
    {{{{16, 8, 0}, {8, 8, 0}, {0, 8, 0}, {24, 8, 0}}}, 4, false},
    {{{{16, 16, 0x8000}, {0, 16, 0x8000}, {}, {}}}, 2, false},
    {{{{0, 10, 0x200}, {10, 10, 0x200}, {20, 10, 0x200}, {30, 2, 0}}}, 4, false},
    {{{{16, 16, 0}, {0, 16, 0}, {}, {}}}, 2, true},
    {{{{48, 16, 0x8000}, {32, 16, 0x8000}, {16, 16, 0x8000}, {0, 16, 0x8000}}}, 4, false},
    {{{{48, 16, 0}, {32, 16, 0}, {16, 16, 0}, {0, 16, 0}}}, 4, true},
    {{{{0, 20, 0x80000}, {20, 20, 0x80000}, {40, 20, 0x80000}, {60, 4, 0}}}, 4, false},
}};

/**
 * Converts the 3.0 + k form of a vector element to a field.
 *
 * @param bits The element bits.
 * @param range The integer range the field accepts.
 * @param lane The field placement.
 * @param saturated Receives 1 when the element lay outside the range.
 * @return The field, clamped to the range, or the NaN field for a NaN.
 */
inline u64 PackInteger(u32 bits, const PackRange& range, const PackField& lane, u8& saturated) {
  const u64 mask = (u64{1} << lane.width) - 1;
  if ((bits & 0x7FFFFFFFu) > 0x7F800000u) {
    return lane.nan_field;
  }
  i64 k = (bits >> 31) != 0 ? -0x100000000LL : static_cast<i64>(bits) - 0x40400000LL;
  if (range.checked) {
    const i64 low = static_cast<i64>(range.low);
    const i64 high = static_cast<i64>(range.high);
    if (k < low || k > high) {
      saturated = 1;
      k = k < low ? low : high;
    }
  }
  return static_cast<u64>(k) & mask;
}

/**
 * Converts a vector element to a 16 bit float, truncating the mantissa, flushing a denormal result
 * to a signed zero and giving an overflow or a NaN every mantissa bit set.
 *
 * @param bits The element bits.
 * @return The float16 bits.
 */
inline u64 PackHalf(u32 bits) {
  const u32 sign = (bits >> 16) & 0x8000u;
  const u32 magnitude = bits & 0x7FFFFFFFu;
  if (magnitude > 0x477FE000u) {
    return sign | 0x7FFFu;
  }
  const u32 exponent = magnitude >> 23;
  if (exponent <= 0x70u) {
    return sign;
  }
  return sign | (((exponent - 0x70u) << 10) + ((magnitude & 0x7FE000u) >> 13));
}

/**
 * Packs the 4 elements of a vector into the 64 bit result of a pack format.
 *
 * @tparam Type The pack format, 0 to 6.
 * @param source The source register.
 * @param saturated Receives 1 when an element lay outside the range of its field.
 * @return The packed result with the low word in the low 32 bits.
 */
template <u32 Type>
inline u64 PackElements(const VRegister& source, u8& saturated) {
  if constexpr (Type == 0) {
    const auto bits = simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(source.u32));
    const auto offset = simde_mm_sub_epi32(bits, simde_mm_set1_epi32(0x40400000));
    const auto negative = simde_mm_srai_epi32(bits, 31);
    const auto below =
        simde_mm_or_si128(negative, simde_mm_cmpgt_epi32(simde_mm_setzero_si128(), offset));
    const auto above =
        simde_mm_andnot_si128(below, simde_mm_cmpgt_epi32(offset, simde_mm_set1_epi32(255)));
    const auto not_a_number = simde_mm_cmpgt_epi32(
        simde_mm_and_si128(bits, simde_mm_set1_epi32(0x7FFFFFFF)), simde_mm_set1_epi32(0x7F800000));
    const auto outside = simde_mm_andnot_si128(not_a_number, simde_mm_or_si128(below, above));
    saturated = simde_mm_movemask_epi8(outside) != 0 ? 1 : 0;
    const auto clamped =
        simde_mm_andnot_si128(simde_mm_or_si128(below, not_a_number),
                              simde_mm_or_si128(simde_mm_and_si128(above, simde_mm_set1_epi32(255)),
                                                simde_mm_andnot_si128(above, offset)));
    const auto bytes = simde_mm_shuffle_epi8(
        clamped, simde_mm_set_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 12, 8, 4));
    return static_cast<u32>(simde_mm_cvtsi128_si32(bytes));
  } else {
    constexpr const detail::PackLayout& kLayout = detail::kPackLayouts[Type];
    u64 packed = 0;
    for (u32 i = 0; i < kLayout.count; ++i) {
      const u32 bits = source.u32[3 - i];
      const detail::PackField& lane = kLayout.lanes[i];
      const u64 field = kLayout.half ? detail::PackHalf(bits)
                                     : detail::PackInteger(bits, detail::kPackD3DRanges[Type][i],
                                                           lane, saturated);
      packed |= field << lane.shift;
    }
    return packed;
  }
}

}  // namespace detail

/**
 * Packs a vector the way vpkd3d128 does for a pack format, a mask and a shift.
 *
 * The source is read before the destination is written, so both may be the same
 * register. Element 0 is word index 3 of the register.
 *
 * @tparam Type The pack format, 0 to 6.
 * @tparam Mask The vpkd3d128 mask, 0 to 3.
 * @tparam Shift The vpkd3d128 shift, 0 to 3.
 * @param dest The destination register.
 * @param source The source register.
 * @return 1 when an element lay outside the range of its field, otherwise 0.
 */
template <u32 Type, u32 Mask, u32 Shift>
inline u8 PackD3D(VRegister& dest, const VRegister& source) {
  u8 saturated = 0;
  const u64 packed = detail::PackElements<Type>(source, saturated);
  const u32 lo = static_cast<u32>(packed);
  const u32 hi = static_cast<u32>(packed >> 32);
  if constexpr (Mask == 3 && Shift == 3) {
    dest.u32[0] = hi;
  } else {
    dest.u32[Shift] = lo;
    if constexpr (Mask >= 2 && Shift < 3) {
      dest.u32[Shift + 1] = hi;
    }
  }
  return saturated;
}

}  // namespace rex::ppc::simd
