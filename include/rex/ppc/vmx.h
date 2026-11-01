/**
 * @file        ppc/vmx.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

/*
 * Parts of the SIMD patterns are based on XenonRecomp and UnleashedRecomp.
 */

#pragma once

#include <array>
#include <climits>
#include <cmath>
#include <cstring>

#include <type_traits>

#include <simde/x86/avx.h>
#include <simde/x86/avx2.h>
#include <simde/x86/sse.h>
#include <simde/x86/sse4.1.h>

#include <rex/ppc/context.h>
#include <rex/types.h>

namespace rex::ppc::simd {

/**
 * Tables for the lvlx, lvrx, stvlx and stvrx vector load and store instructions, 16 rows of 16
 * bytes indexed by the low 4 address bits.
 * @{
 */
alignas(16) inline constexpr u8 kVectorMaskL[] = {
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F,
};

alignas(16) inline constexpr u8 kVectorMaskR[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF,
    0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF,
    0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF,
};

alignas(16) inline constexpr u8 kVectorShiftTableL[] = {
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C,
    0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D,
    0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E,
    0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F,
};

alignas(16) inline constexpr u8 kVectorShiftTableR[] = {
    0x1F, 0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10,
    0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F,
    0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E,
    0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D,
    0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C,
    0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
};

template <typename T>
using VectorOf = std::conditional_t<std::is_floating_point_v<T>, simde__m128, simde__m128i>;

/** Adds the lanes of two vectors with saturation. */
template <typename T>
simde__m128i AddSaturate(simde__m128i a, simde__m128i b);

/** Adds unsigned 32 bit lanes with saturation. */
template <>
inline simde__m128i AddSaturate<u32>(simde__m128i a, simde__m128i b) {
  return simde_mm_add_epi32(
      a, simde_mm_min_epu32(simde_mm_xor_si128(a, simde_mm_cmpeq_epi32(a, a)), b));
}

/** Averages the lanes of two vectors. */
template <typename T>
simde__m128i Average(simde__m128i a, simde__m128i b);

/** Averages signed 8 bit lanes, rounding toward positive infinity. */
template <>
inline simde__m128i Average<i8>(simde__m128i a, simde__m128i b) {
  simde__m128i c = simde_mm_set1_epi8(static_cast<char>(128));
  return simde_mm_xor_si128(c,
                            simde_mm_avg_epu8(simde_mm_xor_si128(c, a), simde_mm_xor_si128(c, b)));
}

/** Averages signed 16 bit lanes. */
template <>
inline simde__m128i Average<i16>(simde__m128i a, simde__m128i b) {
  simde__m128i c = simde_mm_set1_epi16(static_cast<short>(32768));
  return simde_mm_xor_si128(c,
                            simde_mm_avg_epu16(simde_mm_xor_si128(c, a), simde_mm_xor_si128(c, b)));
}

/** Averages signed 32 bit lanes. */
template <>
inline simde__m128i Average<i32>(simde__m128i a, simde__m128i b) {
  simde__m128i sum = simde_mm_add_epi32(simde_mm_srai_epi32(a, 1), simde_mm_srai_epi32(b, 1));
  return simde_mm_add_epi32(sum,
                            simde_mm_and_si128(simde_mm_or_si128(a, b), simde_mm_set1_epi32(1)));
}

/** Converts the lanes of a vector to another lane type. */
template <typename To, typename From>
VectorOf<To> Convert(VectorOf<From> src);

/** Converts unsigned 32 bit integers to floats. */
template <>
inline simde__m128 Convert<f32, u32>(simde__m128i src1) {
  simde__m128i xmm1 = simde_mm_add_epi32(src1, simde_mm_set1_epi32(127));
  simde__m128i xmm0 = simde_mm_slli_epi32(src1, 31 - 8);
  xmm0 = simde_mm_srli_epi32(xmm0, 31);
  xmm0 = simde_mm_add_epi32(xmm0, xmm1);
  xmm0 = simde_mm_srai_epi32(xmm0, 8);
  xmm0 = simde_mm_add_epi32(xmm0, simde_mm_set1_epi32(0x4F800000));
  simde__m128 xmm2 = simde_mm_cvtepi32_ps(src1);
  return simde_mm_blendv_ps(xmm2, simde_mm_castsi128_ps(xmm0), simde_mm_castsi128_ps(src1));
}

inline simde__m128i PermuteBytes(simde__m128i a, simde__m128i b, simde__m128i c) {
  simde__m128i d = simde_mm_set1_epi8(0xF);
  simde__m128i e = simde_mm_sub_epi8(d, simde_mm_and_si128(c, d));
  return simde_mm_blendv_epi8(simde_mm_shuffle_epi8(a, e), simde_mm_shuffle_epi8(b, e),
                              simde_mm_slli_epi32(c, 3));
}

/**
 * Compares the lanes of two vectors for greater than.
 *
 * @return All ones in lanes where a is greater than b.
 */
template <typename T>
simde__m128i CompareGreater(simde__m128i a, simde__m128i b);

/**
 * Compares unsigned 8 bit lanes for greater than.
 *
 * @return All ones in lanes where a is greater than b.
 */
template <>
inline simde__m128i CompareGreater<u8>(simde__m128i a, simde__m128i b) {
  simde__m128i c = simde_mm_set1_epi8(static_cast<char>(128));
  return simde_mm_cmpgt_epi8(simde_mm_xor_si128(a, c), simde_mm_xor_si128(b, c));
}

/**
 * Compares unsigned 16 bit lanes for greater than.
 *
 * @return All ones in lanes where a is greater than b.
 */
template <>
inline simde__m128i CompareGreater<u16>(simde__m128i a, simde__m128i b) {
  simde__m128i c = simde_mm_set1_epi16(static_cast<short>(32768));
  return simde_mm_cmpgt_epi16(simde_mm_xor_si128(a, c), simde_mm_xor_si128(b, c));
}

/** Converts the lanes of a vector to another lane type with saturation. */
template <typename To, typename From>
VectorOf<To> ConvertSaturate(VectorOf<From> src);

/** Converts floats to signed fixed point words with saturation (vctsxs). */
template <>
inline simde__m128i ConvertSaturate<i32, f32>(simde__m128 src1) {
  simde__m128 xmm2 = simde_mm_cmpunord_ps(src1, src1);
  simde__m128i xmm0 = simde_mm_cvttps_epi32(src1);
  simde__m128i xmm1 = simde_mm_cmpeq_epi32(xmm0, simde_mm_set1_epi32(INT_MIN));
  xmm1 = simde_mm_andnot_si128(simde_mm_castps_si128(src1), xmm1);
  simde__m128 dest = simde_mm_blendv_ps(simde_mm_castsi128_ps(xmm0),
                                        simde_mm_castsi128_ps(simde_mm_set1_epi32(INT_MAX)),
                                        simde_mm_castsi128_ps(xmm1));
  return simde_mm_andnot_si128(simde_mm_castps_si128(xmm2), simde_mm_castps_si128(dest));
}

/**
 * Converts floats to unsigned fixed point words with saturation (vctuxs).
 *
 * NaN and negative inputs give 0 and inputs above UINT_MAX give UINT_MAX.
 *
 * @return The words, saturated to [0, UINT_MAX].
 */
template <>
inline simde__m128i ConvertSaturate<u32, f32>(simde__m128 src1) {
  simde__m128 nan_mask = simde_mm_cmpunord_ps(src1, src1);
  simde__m128 neg_mask = simde_mm_cmplt_ps(src1, simde_mm_setzero_ps());
  simde__m128 max_val = simde_mm_set1_ps(4294967295.0f);
  simde__m128 overflow_mask = simde_mm_cmpge_ps(src1, max_val);

  simde__m128 clamped = simde_mm_max_ps(src1, simde_mm_setzero_ps());
  clamped = simde_mm_min_ps(clamped, max_val);

  simde__m128 half_range = simde_mm_set1_ps(2147483648.0f);
  simde__m128 high_bit_mask = simde_mm_cmpge_ps(clamped, half_range);

  simde__m128 adjusted = simde_mm_sub_ps(clamped, simde_mm_and_ps(high_bit_mask, half_range));
  simde__m128i low_bits = simde_mm_cvttps_epi32(adjusted);
  simde__m128i high_bit = simde_mm_and_si128(simde_mm_castps_si128(high_bit_mask),
                                             simde_mm_set1_epi32(static_cast<int>(0x80000000u)));
  simde__m128i result = simde_mm_or_si128(low_bits, high_bit);

  result = simde_mm_andnot_si128(simde_mm_castps_si128(nan_mask), result);
  result = simde_mm_andnot_si128(simde_mm_castps_si128(neg_mask), result);
  result = simde_mm_or_si128(
      simde_mm_andnot_si128(simde_mm_castps_si128(overflow_mask), result),
      simde_mm_and_si128(simde_mm_castps_si128(overflow_mask), simde_mm_set1_epi32(-1)));

  return result;
}

/**
 * Shifts the whole 128 bit vector right (vsr).
 *
 * @param b The shift count source.
 */
inline simde__m128i ShiftRightBits(simde__m128i a, simde__m128i b) {
  b = simde_mm_srli_epi64(simde_mm_slli_epi64(b, 61), 61);
  return simde_mm_castps_si128(simde_mm_insert_ps(
      simde_mm_castsi128_ps(simde_mm_srl_epi64(a, b)),
      simde_mm_castsi128_ps(simde_mm_srl_epi64(simde_mm_srli_si128(a, 4), b)), 0x10));
}

/**
 * Shifts the whole 128 bit vector left (vsl) by the low 3 bits of b.
 *
 * @param b The shift count source, read from byte 15 which is byte 0 in big endian.
 */
inline simde__m128i ShiftLeftBits(simde__m128i a, simde__m128i b) {
  int shift = simde_mm_extract_epi8(b, 15) & 0x7;
  if (shift == 0)
    return a;

  simde__m128i low_shifted = simde_mm_sll_epi64(a, simde_mm_cvtsi32_si128(shift));
  simde__m128i high_carry = simde_mm_srl_epi64(a, simde_mm_cvtsi32_si128(64 - shift));
  high_carry = simde_mm_slli_si128(high_carry, 8);
  return simde_mm_or_si128(low_shifted, high_carry);
}

/**
 * Shifts the whole vector left by bytes (vslo) given by bits [121:124] of b.
 *
 * @param b The byte count source.
 */
inline simde__m128i ShiftLeftBytes(simde__m128i a, simde__m128i b) {
  int shift_bytes = (simde_mm_extract_epi8(b, 0) >> 3) & 0xF;
  if (shift_bytes == 0)
    return a;
  if (shift_bytes >= 16)
    return simde_mm_setzero_si128();

  alignas(16) u8 src[16], dst[16];
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(src), a);
  memset(dst, 0, sizeof(dst));
  memcpy(dst + shift_bytes, src, 16 - shift_bytes);
  return simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(dst));
}

/**
 * Shifts the whole vector right by bytes (vsro) given by bits [121:124] of b.
 *
 * @param b The byte count source.
 */
inline simde__m128i ShiftRightBytes(simde__m128i a, simde__m128i b) {
  int shift_bytes = (simde_mm_extract_epi8(b, 0) >> 3) & 0xF;
  if (shift_bytes == 0)
    return a;
  if (shift_bytes >= 16)
    return simde_mm_setzero_si128();

  alignas(16) u8 src[16], dst[16];
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(src), a);
  memset(dst, 0, sizeof(dst));
  memcpy(dst, src + shift_bytes, 16 - shift_bytes);
  return simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(dst));
}

/** Shifts each lane left by the count in the same lane. */
template <typename T>
simde__m128i ShiftLeftLanes(simde__m128i a, simde__m128i count);

/** Shifts 16 bit lanes left by per lane counts. */
template <>
inline simde__m128i ShiftLeftLanes<u16>(simde__m128i a, simde__m128i count) {
  simde__m128i zero = simde_mm_setzero_si128();
  simde__m128i a_lo = simde_mm_unpacklo_epi16(a, zero);
  simde__m128i a_hi = simde_mm_unpackhi_epi16(a, zero);
  simde__m128i s_lo = simde_mm_unpacklo_epi16(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi16(count, zero);
  simde__m128i r_lo = simde_mm_sllv_epi32(a_lo, s_lo);
  simde__m128i r_hi = simde_mm_sllv_epi32(a_hi, s_hi);
  simde__m128i mask16 = simde_mm_set1_epi32(0xFFFF);
  r_lo = simde_mm_and_si128(r_lo, mask16);
  r_hi = simde_mm_and_si128(r_hi, mask16);
  return simde_mm_packus_epi32(r_lo, r_hi);
}

/**
 * Shifts each lane right by the count in the same lane. A signed lane type shifts arithmetically.
 */
template <typename T>
simde__m128i ShiftRightLanes(simde__m128i a, simde__m128i count);

/** Shifts 16 bit lanes right logically by per lane counts. */
template <>
inline simde__m128i ShiftRightLanes<u16>(simde__m128i a, simde__m128i count) {
  simde__m128i zero = simde_mm_setzero_si128();
  simde__m128i a_lo = simde_mm_unpacklo_epi16(a, zero);
  simde__m128i a_hi = simde_mm_unpackhi_epi16(a, zero);
  simde__m128i s_lo = simde_mm_unpacklo_epi16(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi16(count, zero);
  simde__m128i r_lo = simde_mm_srlv_epi32(a_lo, s_lo);
  simde__m128i r_hi = simde_mm_srlv_epi32(a_hi, s_hi);
  return simde_mm_packus_epi32(r_lo, r_hi);
}

/** Shifts 16 bit lanes right arithmetically by per lane counts. */
template <>
inline simde__m128i ShiftRightLanes<i16>(simde__m128i a, simde__m128i count) {
  simde__m128i zero = simde_mm_setzero_si128();
  simde__m128i a_lo = simde_mm_srai_epi32(simde_mm_unpacklo_epi16(a, a), 16);
  simde__m128i a_hi = simde_mm_srai_epi32(simde_mm_unpackhi_epi16(a, a), 16);
  simde__m128i s_lo = simde_mm_unpacklo_epi16(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi16(count, zero);
  simde__m128i r_lo = simde_mm_srav_epi32(a_lo, s_lo);
  simde__m128i r_hi = simde_mm_srav_epi32(a_hi, s_hi);
  return simde_mm_packs_epi32(r_lo, r_hi);
}

/** Shifts 8 bit lanes left by per lane counts. */
template <>
inline simde__m128i ShiftLeftLanes<u8>(simde__m128i a, simde__m128i count) {
  simde__m128i zero = simde_mm_setzero_si128();
  simde__m128i a_lo = simde_mm_unpacklo_epi8(a, zero);
  simde__m128i a_hi = simde_mm_unpackhi_epi8(a, zero);
  simde__m128i s_lo = simde_mm_unpacklo_epi8(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi8(count, zero);
  simde__m128i r_lo = ShiftLeftLanes<u16>(a_lo, s_lo);
  simde__m128i r_hi = ShiftLeftLanes<u16>(a_hi, s_hi);
  simde__m128i mask8 = simde_mm_set1_epi16(0xFF);
  r_lo = simde_mm_and_si128(r_lo, mask8);
  r_hi = simde_mm_and_si128(r_hi, mask8);
  return simde_mm_packus_epi16(r_lo, r_hi);
}

/** Reverses the 16 bytes of a vector register into guest byte order. */
inline simde__m128i ToGuestByteOrder(simde__m128i v) {
  return simde_mm_shuffle_epi8(
      v, simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(kVectorMaskL)));
}

/**
 * Writes the first count bytes of a 16 byte buffer to host memory, touching no other byte.
 *
 * @param bytes The 16 byte source in guest memory order.
 * @param count The number of bytes to write, 0 to 16.
 */
inline void StoreVectorBytes(u8* host, const u8* bytes, u32 count) {
  if (count == 16) {
    simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(host),
                          simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(bytes)));
  } else if (count >= 8) {
    std::memcpy(host, bytes, 8);
    std::memcpy(host + count - 8, bytes + count - 8, 8);
  } else if (count >= 4) {
    std::memcpy(host, bytes, 4);
    std::memcpy(host + count - 4, bytes + count - 4, 4);
  } else if (count >= 2) {
    std::memcpy(host, bytes, 2);
    std::memcpy(host + count - 2, bytes + count - 2, 2);
  } else if (count == 1) {
    host[0] = bytes[0];
  }
}

/**
 * Stores the first 16 - (ea & 0xF) guest order bytes of a vector, the stvlx operation.
 *
 * @param host The host address of ea.
 */
inline void StoreVectorLeft(u8* host, u32 ea, simde__m128i v) {
  alignas(16) u8 bytes[16];
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(bytes), ToGuestByteOrder(v));
  StoreVectorBytes(host, bytes, 16 - (ea & 0xF));
}

/**
 * Stores the last ea & 0xF guest order bytes of a vector at the start of the quadword that holds
 * ea, the stvrx operation. Writes nothing when ea is aligned.
 *
 * @param host The host address of the start of that quadword.
 */
inline void StoreVectorRight(u8* host, u32 ea, simde__m128i v) {
  alignas(16) u8 bytes[16];
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(bytes), ToGuestByteOrder(v));
  const u32 count = ea & 0xF;
  StoreVectorBytes(host, bytes + 16 - count, count);
}

/**
 * Stores a vector with the stvlx and stvrx pair used for unaligned stores.
 *
 * @param left_host The host address of the stvlx address.
 * @param left_ea The stvlx guest effective address.
 * @param right_host The host address of the start of the quadword that holds the stvrx address.
 * @param right_ea The stvrx guest effective address.
 */
inline void StoreVectorPair(u8* left_host, u32 left_ea, u8* right_host, u32 right_ea,
                            simde__m128i v) {
  if (right_ea == left_ea + 16 && right_host == left_host + (16 - (left_ea & 0xF))) {
    simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(left_host), ToGuestByteOrder(v));
    return;
  }
  StoreVectorLeft(left_host, left_ea, v);
  StoreVectorRight(right_host, right_ea, v);
}

/** @} */

}  // namespace rex::ppc::simd

/** Vector tables at global scope for generated code. @{ */
using rex::ppc::simd::kVectorMaskL;
using rex::ppc::simd::kVectorMaskR;
using rex::ppc::simd::kVectorShiftTableL;
using rex::ppc::simd::kVectorShiftTableR;
/** @} */

namespace rex::ppc::simd {

/**
 * Reports whether a float to fixed point conversion clamps any lane, not counting a NaN lane.
 *
 * @tparam Signed True for the signed word conversion, false for the unsigned one.
 * @param scaled The floats after the scale multiply.
 * @return True when a lane lies outside the range of the destination word.
 */
template <bool Signed>
[[nodiscard]] inline bool IsConvertSaturating(simde__m128 scaled) noexcept {
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
 * @return True when at least one lane differs.
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
 * @return True when a lane overflows its element range.
 */
template <u32 Bits, bool Signed>
[[nodiscard]] inline bool IsAddSaturating(simde__m128i a, simde__m128i b) noexcept {
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
[[nodiscard]] inline bool IsSubSaturating(simde__m128i a, simde__m128i b) noexcept {
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
 * Reports whether a saturating vector pack clamps any lane of its two operands.
 *
 * @tparam SourceBits The source element width, 16 or 32.
 * @tparam SignedDest True when the packed elements are signed.
 * @return True when a source element lies outside the range of the packed element.
 */
template <u32 SourceBits, bool SignedDest>
[[nodiscard]] inline bool IsPackSaturating(simde__m128i a, simde__m128i b) noexcept {
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

/** The integer range of each vector element, indexed by pack format. */
inline constexpr std::array<std::array<PackRange, 4>, 7> kPackD3DRanges = {{
    {kByteRange, kByteRange, kByteRange, kByteRange},
    {kShortRange, kShortRange, kUnchecked, kUnchecked},
    {kSigned10Range, kSigned10Range, kSigned10Range, kTwoBitRange},
    {kUnchecked, kUnchecked, kUnchecked, kUnchecked},
    {kShortRange, kShortRange, kShortRange, kShortRange},
    {kUnchecked, kUnchecked, kUnchecked, kUnchecked},
    {kSigned20Range, kSigned20Range, kSigned20Range, kFourBitRange},
}};

/** The lowest or highest float of each host lane range, or 0 for a lane that never saturates. */
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
 * shift write, and a NaN element never saturates.
 *
 * @tparam Type The pack format, 0 to 6.
 * @return True when an element lies outside the range of its packed field.
 */
template <u32 Type>
[[nodiscard]] inline bool IsPackD3DSaturating(simde__m128 source) noexcept {
  const auto low = simde_mm_loadu_ps(detail::kPackD3DBounds<Type, false>.data());
  const auto high = simde_mm_loadu_ps(detail::kPackD3DBounds<Type, true>.data());
  const auto checked =
      simde_mm_loadu_ps(reinterpret_cast<const f32*>(detail::kPackD3DChecked<Type>.data()));
  const auto inside =
      simde_mm_and_ps(simde_mm_cmpge_ps(source, low), simde_mm_cmple_ps(source, high));
  const auto outside = simde_mm_andnot_ps(inside, simde_mm_cmpord_ps(source, source));
  return simde_mm_movemask_ps(simde_mm_and_ps(outside, checked)) != 0;
}

}  // namespace rex::ppc::simd

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
 * to a signed zero and setting every mantissa bit for an overflow or a NaN.
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
 * Packs the four elements of a vector into the 64 bit result of a pack format.
 *
 * @tparam Type The pack format, 0 to 6.
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
 * Packs a vector the way vpkd3d128 does. Element 0 is word index 3 of the register, and dest may
 * be source.
 *
 * @tparam Type The pack format, 0 to 6.
 * @tparam Mask The vpkd3d128 mask, 0 to 3.
 * @tparam Shift The vpkd3d128 shift, 0 to 3.
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
