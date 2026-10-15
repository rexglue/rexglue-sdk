/**
 * @file        ppc/estimate.h
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

#include <rex/ppc/estimate_tables.h>
#include <rex/types.h>

namespace rex::ppc {

/** Sign bit of an f32 operand. */
inline constexpr u32 kSign32 = 0x80000000u;
/** Quiet bit of an f32 NaN. */
inline constexpr u32 kQuiet32 = 0x00400000u;
/** Positive infinity as f32 bits. */
inline constexpr u32 kInfinity32 = 0x7F800000u;
/** Default quiet NaN as f32 bits. */
inline constexpr u32 kQNaN32 = 0x7FC00000u;
/** Sign bit of an f64 operand. */
inline constexpr u64 kSign64 = 0x8000000000000000ull;
/** Quiet bit of an f64 NaN. */
inline constexpr u64 kQuiet64 = 0x0008000000000000ull;
/** Mantissa mask of an f64 operand. */
inline constexpr u64 kMantissa64 = 0x000FFFFFFFFFFFFFull;
/** Positive infinity as f64 bits. */
inline constexpr u64 kInfinity64 = 0x7FF0000000000000ull;
/** Default quiet NaN as f64 bits. */
inline constexpr u64 kQNaN64 = 0x7FF8000000000000ull;

/**
 * Computes the scalar reciprocal estimate of fres.
 *
 * The result is the table value for the top 7 mantissa bits with the exponent negated. Operands
 * with a biased exponent below 895 give a signed infinity, above 1172 a signed zero. A NaN comes
 * back quiet with its sign and payload.
 *
 * @param x The operand.
 * @return The estimate.
 */
[[nodiscard]] inline f64 Fres(f64 x) noexcept {
  const u64 a = std::bit_cast<u64>(x);
  const u64 sign = a & kSign64;
  const i64 e = static_cast<i64>((a >> 52) & 0x7FFu);
  const u64 m = a & kMantissa64;
  if (e == 0x7FF) {
    return std::bit_cast<f64>(m != 0 ? a | kQuiet64 : sign);
  }
  if (e < 895) {
    return std::bit_cast<f64>(sign | kInfinity64);
  }
  if (e >= 1173) {
    return std::bit_cast<f64>(sign);
  }
  const u64 base = kFresTable[m >> 45];
  const i64 exponent = static_cast<i64>(base >> 52) - (e - 1023);
  return std::bit_cast<f64>(sign | static_cast<u64>(exponent) << 52 | (base & kMantissa64));
}

/**
 * Computes the scalar reciprocal square root estimate of frsqrte.
 *
 * The result is the table value for the exponent parity and top 3 mantissa bits with half the
 * exponent negated. A denormal operand is normalized first. Zero gives a signed infinity, and a
 * negative operand or negative infinity gives the default quiet NaN.
 *
 * @param x The operand.
 * @return The estimate.
 */
[[nodiscard]] inline f64 Frsqrte(f64 x) noexcept {
  const u64 a = std::bit_cast<u64>(x);
  const u64 sign = a & kSign64;
  i64 e = static_cast<i64>((a >> 52) & 0x7FFu);
  u64 m = a & kMantissa64;
  if (e == 0x7FF) {
    return std::bit_cast<f64>(m != 0 ? a | kQuiet64 : (sign != 0 ? kQNaN64 : 0));
  }
  if (e == 0) {
    if (m == 0) {
      return std::bit_cast<f64>(sign | kInfinity64);
    }
    const i32 shift = std::countl_zero(m) - 11;
    m = (m << shift) & kMantissa64;
    e = 1 - shift;
  }
  if (sign != 0) {
    return std::bit_cast<f64>(kQNaN64);
  }
  const i64 unbiased = e - 1023;
  const u64 base = kFrsqrteTable[static_cast<u64>(unbiased & 1) << 3 | m >> 49];
  const i64 exponent = static_cast<i64>(base >> 52) - (unbiased >> 1);
  return std::bit_cast<f64>(static_cast<u64>(exponent) << 52 | (base & kMantissa64));
}

namespace detail {

/** The bytes of 1 plane of a 32 entry table, split in the 2 halves a byte shuffle indexes. */
struct Plane32 {
  u8 lo[16];
  u8 hi[16];
};

/** The bytes of 1 plane of a 16 entry table. */
struct Plane16 {
  u8 bytes[16];
};

/** Packs the base and the slope magnitude of a vrefp cell. */
constexpr u32 VrefpCell(u32 i) {
  return (kVrefpBase[i] >> 9) << 10 | static_cast<u32>(-kVrefpSlope[i] / 4);
}

/** Packs the base and the slope magnitude of a vrsqrtefp cell. */
constexpr u32 VrsqrtefpCell(u32 i) {
  return (kVrsqrtefpBase[i] >> 8) << 11 | static_cast<u32>(-kVrsqrtefpSlope[i]);
}

/** Packs the base and the slope of a vlogefp cell. */
constexpr u32 VlogefpCell(u32 i) {
  return static_cast<u32>(kVlogefpBase[i]) << 4 | static_cast<u32>(kVlogefpSlope[i]);
}

/** Finds the base and the slope of the line that reproduces 128 entries of the exp2 table. */
constexpr u32 VexptefpCell(u32 i) {
  for (u32 slope = 0; slope < 16; ++slope) {
    i32 lowest = -0x7FFFFFFF;
    i32 highest = 0x7FFFFFFF;
    for (u32 u = 0; u < 128; ++u) {
      const i32 entry = 8 * static_cast<i32>(kVexptefpPositive[i * 128 + u]);
      const i32 offset = static_cast<i32>(slope * u);
      lowest = lowest > entry - offset ? lowest : entry - offset;
      highest = highest < entry + 7 - offset ? highest : entry + 7 - offset;
    }
    if (lowest <= highest) {
      return static_cast<u32>(lowest) << 4 | slope;
    }
  }
  return 0;
}

constexpr bool IsExactLine() {
  for (u32 i = 0; i < 16; ++i) {
    const u32 cell = VexptefpCell(i);
    for (u32 u = 0; u < 128; ++u) {
      const u32 value = ((cell >> 4) + (cell & 15) * u) >> 3;
      if (value != kVexptefpPositive[i * 128 + u]) {
        return false;
      }
    }
  }
  return true;
}

static_assert(IsExactLine(), "the exp2 table must follow 16 lines");

template <u32 (*Cell)(u32), u32 Byte>
inline constexpr Plane32 kPlane32 = [] {
  Plane32 plane{};
  for (u32 i = 0; i < 16; ++i) {
    plane.lo[i] = static_cast<u8>(Cell(i) >> (8 * Byte));
    plane.hi[i] = static_cast<u8>(Cell(i + 16) >> (8 * Byte));
  }
  return plane;
}();

template <u32 (*Cell)(u32), u32 Byte>
inline constexpr Plane16 kPlane16 = [] {
  Plane16 plane{};
  for (u32 i = 0; i < 16; ++i) {
    plane.bytes[i] = static_cast<u8>(Cell(i) >> (8 * Byte));
  }
  return plane;
}();

inline simde__m128i Set(u32 value) {
  return simde_mm_set1_epi32(static_cast<i32>(value));
}

inline simde__m128i Select(simde__m128i mask, simde__m128i yes, simde__m128i no) {
  return simde_mm_blendv_epi8(no, yes, mask);
}

inline simde__m128i Equal(simde__m128i a, u32 value) {
  return simde_mm_cmpeq_epi32(a, Set(value));
}

/** The shuffle controls of a lookup, that put the table byte of each lane index in byte 0. */
struct Controls {
  simde__m128i lo;
  simde__m128i hi;
};

inline Controls MakeControls(simde__m128i index) {
  const auto lo = simde_mm_or_si128(simde_mm_add_epi32(index, Set(0x70)), Set(0x80808000u));
  return {lo, simde_mm_xor_si128(lo, Set(0x80))};
}

/** Reads 1 plane of a 32 entry table into byte 0 of each lane. */
template <const Plane32& Plane>
inline simde__m128i Fetch32(const Controls& controls) {
  const auto lo = simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(Plane.lo));
  const auto hi = simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(Plane.hi));
  return simde_mm_or_si128(simde_mm_shuffle_epi8(lo, controls.lo),
                           simde_mm_shuffle_epi8(hi, controls.hi));
}

/** Reads 1 plane of a 16 entry table into byte 0 of each lane. */
template <const Plane16& Plane>
inline simde__m128i Fetch16(simde__m128i control) {
  const auto bytes = simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(Plane.bytes));
  return simde_mm_shuffle_epi8(bytes, control);
}

/** Reads the cell of a 32 entry table for the index of each lane. */
template <u32 (*Cell)(u32), u32 Planes>
inline simde__m128i Lookup32(simde__m128i index) {
  const auto controls = MakeControls(index);
  const auto byte0 = Fetch32<kPlane32<Cell, 0>>(controls);
  const auto byte1 = Fetch32<kPlane32<Cell, 1>>(controls);
  const auto byte2 = Fetch32<kPlane32<Cell, 2>>(controls);
  auto cell = simde_mm_or_si128(byte0, simde_mm_slli_epi32(byte1, 8));
  cell = simde_mm_or_si128(cell, simde_mm_slli_epi32(byte2, 16));
  if constexpr (Planes > 3) {
    const auto byte3 = Fetch32<kPlane32<Cell, 3>>(controls);
    cell = simde_mm_or_si128(cell, simde_mm_slli_epi32(byte3, 24));
  }
  return cell;
}

/** Reads the cell of a 16 entry table for the index of each lane. */
template <u32 (*Cell)(u32)>
inline simde__m128i Lookup16(simde__m128i index) {
  const auto control = simde_mm_or_si128(index, Set(0x80808000u));
  const auto byte0 = Fetch16<kPlane16<Cell, 0>>(control);
  const auto byte1 = Fetch16<kPlane16<Cell, 1>>(control);
  const auto byte2 = Fetch16<kPlane16<Cell, 2>>(control);
  const auto cell = simde_mm_or_si128(byte0, simde_mm_slli_epi32(byte1, 8));
  return simde_mm_or_si128(cell, simde_mm_slli_epi32(byte2, 16));
}

/** Reports whether any lane of the value is below the lowest or above the highest bound. */
inline bool AnyOutside(simde__m128i value, u32 lowest, u32 highest) {
  const auto offset = simde_mm_add_epi32(value, Set(kSign32 - lowest));
  const auto outside = simde_mm_cmpgt_epi32(offset, Set(kSign32 + (highest - lowest)));
  return simde_mm_testz_si128(outside, outside) == 0;
}

}  // namespace detail

/**
 * Computes vrefp and vrefp128.
 *
 * @param v The operand.
 * @return The reciprocal estimate of each lane.
 */
[[nodiscard]] inline simde__m128 Vrefp(simde__m128 v) noexcept {
  using namespace detail;
  const auto a = simde_mm_castps_si128(v);
  const auto biased = simde_mm_and_si128(a, Set(0x7F800000u));
  const auto cell = Lookup32<VrefpCell, 3>(simde_mm_and_si128(simde_mm_srli_epi32(a, 18), Set(31)));
  const auto u = simde_mm_and_si128(simde_mm_srli_epi32(a, 9), Set(0x1FF));
  const auto base = simde_mm_and_si128(simde_mm_srli_epi32(cell, 1), Set(0x7FFE00));
  const auto mantissa =
      simde_mm_sub_epi32(base, simde_mm_madd_epi16(simde_mm_and_si128(cell, Set(0x3FF)), u));
  const auto top = simde_mm_and_si128(a, Set(0xFF800000u));
  auto result = simde_mm_or_si128(simde_mm_sub_epi32(Set(253u << 23), top), mantissa);
  if (!AnyOutside(biased, 1u << 23, 252u << 23)) [[likely]] {
    return simde_mm_castsi128_ps(result);
  }
  const auto sign = simde_mm_and_si128(a, Set(kSign32));
  const auto e = simde_mm_srli_epi32(biased, 23);
  const auto m = simde_mm_and_si128(a, Set(0x7FFFFF));
  const auto smallest = simde_mm_and_si128(Equal(e, 253), Equal(m, 0));
  const auto underflow = Select(smallest, simde_mm_or_si128(sign, Set(0x00800000u)), sign);
  result = Select(simde_mm_cmpgt_epi32(e, Set(252)), underflow, result);
  const auto nan = simde_mm_andnot_si128(Equal(m, 0), Equal(e, 0xFF));
  result = Select(nan, simde_mm_or_si128(a, Set(kQuiet32)), result);
  return simde_mm_castsi128_ps(
      Select(Equal(e, 0), simde_mm_or_si128(sign, Set(kInfinity32)), result));
}

/**
 * Computes vrsqrtefp and vrsqrtefp128.
 *
 * @param v The operand.
 * @return The reciprocal square root estimate of each lane.
 */
[[nodiscard]] inline simde__m128 Vrsqrtefp(simde__m128 v) noexcept {
  using namespace detail;
  const auto a = simde_mm_castps_si128(v);
  const auto e = simde_mm_srli_epi32(simde_mm_and_si128(a, Set(kInfinity32)), 23);
  const auto index =
      simde_mm_and_si128(simde_mm_srli_epi32(simde_mm_xor_si128(a, Set(1u << 23)), 19), Set(31));
  const auto cell = Lookup32<VrsqrtefpCell, 4>(index);
  const auto u = simde_mm_and_si128(simde_mm_srli_epi32(a, 9), Set(0x3FF));
  const auto product = simde_mm_madd_epi16(simde_mm_and_si128(cell, Set(0x7FF)), u);
  const auto tie = simde_mm_and_si128(simde_mm_srli_epi32(product, 2), Set(1));
  const auto quotient =
      simde_mm_srli_epi32(simde_mm_add_epi32(simde_mm_add_epi32(product, Set(1)), tie), 2);
  const auto mantissa =
      simde_mm_sub_epi32(simde_mm_and_si128(simde_mm_srli_epi32(cell, 3), Set(0x7FFF00)), quotient);
  const auto half = simde_mm_srai_epi32(simde_mm_sub_epi32(e, Set(127)), 1);
  const auto field = simde_mm_slli_epi32(simde_mm_sub_epi32(Set(126), half), 23);
  auto result = simde_mm_or_si128(field, mantissa);
  if (!AnyOutside(a, 1u << 23, (255u << 23) - 1)) [[likely]] {
    return simde_mm_castsi128_ps(result);
  }
  const auto sign = simde_mm_and_si128(a, Set(kSign32));
  const auto negative = simde_mm_srai_epi32(a, 31);
  const auto m = simde_mm_and_si128(a, Set(0x7FFFFF));
  result = Select(negative, Set(kQNaN32), result);
  const auto nan = simde_mm_andnot_si128(Equal(m, 0), Equal(e, 0xFF));
  const auto infinity = simde_mm_and_si128(negative, Set(kQNaN32));
  result =
      Select(Equal(e, 0xFF), Select(nan, simde_mm_or_si128(a, Set(kQuiet32)), infinity), result);
  return simde_mm_castsi128_ps(
      Select(Equal(e, 0), simde_mm_or_si128(sign, Set(kInfinity32)), result));
}

/**
 * Computes vlogefp and vlogefp128.
 *
 * @param v The operand.
 * @return The log2 estimate of each lane.
 */
[[nodiscard]] inline simde__m128 Vlogefp(simde__m128 v) noexcept {
  using namespace detail;
  const auto a = simde_mm_castps_si128(v);
  const auto shifted = simde_mm_srli_epi32(a, 12);
  const auto cell = Lookup16<VlogefpCell>(simde_mm_and_si128(simde_mm_srli_epi32(a, 19), Set(15)));
  const auto u = simde_mm_and_si128(shifted, Set(0x7F));
  const auto fraction = simde_mm_srli_epi32(
      simde_mm_add_epi32(simde_mm_srli_epi32(cell, 4),
                         simde_mm_madd_epi16(simde_mm_and_si128(cell, Set(15)), u)),
      3);
  const auto exponent =
      simde_mm_sub_epi32(simde_mm_and_si128(shifted, Set(0x7F800)), Set(127u << 11));
  const auto fixed = simde_mm_add_epi32(exponent, fraction);
  auto result = simde_mm_castps_si128(
      simde_mm_mul_ps(simde_mm_cvtepi32_ps(fixed), simde_mm_set1_ps(1.0f / 2048.0f)));
  if (!AnyOutside(a, 1u << 23, (255u << 23) - 1)) [[likely]] {
    return simde_mm_castsi128_ps(result);
  }
  const auto negative = simde_mm_srai_epi32(a, 31);
  const auto e = simde_mm_and_si128(simde_mm_srli_epi32(a, 23), Set(0xFF));
  const auto m = simde_mm_and_si128(a, Set(0x7FFFFF));
  result = Select(negative, Set(kQNaN32), result);
  const auto nan = simde_mm_andnot_si128(Equal(m, 0), Equal(e, 0xFF));
  const auto infinity = Select(negative, Set(kQNaN32), a);
  result =
      Select(Equal(e, 0xFF), Select(nan, simde_mm_or_si128(a, Set(kQuiet32)), infinity), result);
  return simde_mm_castsi128_ps(Select(Equal(e, 0), Set(kSign32 | kInfinity32), result));
}

/**
 * Computes vexptefp and vexptefp128.
 *
 * @param v The operand.
 * @return The exp2 estimate of each lane.
 */
[[nodiscard]] inline simde__m128 Vexptefp(simde__m128 v) noexcept {
  using namespace detail;
  const auto a = simde_mm_castps_si128(v);
  const auto fixed =
      simde_mm_srai_epi32(simde_mm_cvttps_epi32(simde_mm_mul_ps(v, simde_mm_set1_ps(32768.0f))), 4);
  const auto fraction = simde_mm_and_si128(fixed, Set(0x7FF));
  const auto cell = Lookup16<VexptefpCell>(simde_mm_srli_epi32(fraction, 7));
  const auto u = simde_mm_and_si128(fraction, Set(0x7F));
  const auto sum = simde_mm_add_epi32(simde_mm_srli_epi32(cell, 4),
                                      simde_mm_madd_epi16(simde_mm_and_si128(cell, Set(15)), u));
  const auto exponent = simde_mm_add_epi32(simde_mm_srai_epi32(fixed, 11), Set(127));
  auto result = simde_mm_or_si128(simde_mm_slli_epi32(exponent, 23),
                                  simde_mm_and_si128(simde_mm_slli_epi32(sum, 9), Set(0x7FF000)));
  if (!AnyOutside(simde_mm_andnot_si128(Set(kSign32), a), 1u << 23, 0x42FBFFFFu)) [[likely]] {
    return simde_mm_castsi128_ps(result);
  }
  const auto negative = simde_mm_srai_epi32(a, 31);
  const auto e = simde_mm_and_si128(simde_mm_srli_epi32(a, 23), Set(0xFF));
  const auto m = simde_mm_and_si128(a, Set(0x7FFFFF));
  result = simde_mm_andnot_si128(simde_mm_cmpgt_epi32(Set(1), exponent), result);
  const auto large = simde_mm_cmpgt_epi32(e, Set(133));
  result = Select(large, simde_mm_andnot_si128(negative, Set(kInfinity32)), result);
  const auto nan = simde_mm_andnot_si128(Equal(m, 0), Equal(e, 0xFF));
  result = Select(nan, simde_mm_or_si128(a, Set(kQuiet32)), result);
  return simde_mm_castsi128_ps(Select(Equal(e, 0), Set(0x3F800000u), result));
}

namespace detail {

/**
 * Reads lane 0 of a vector estimate applied to one operand.
 *
 * @tparam Estimate The vector estimate.
 * @param bits The operand bits.
 * @return The estimate bits.
 */
template <simde__m128 (*Estimate)(simde__m128)>
inline u32 EstimateOne(u32 bits) {
  return static_cast<u32>(simde_mm_cvtsi128_si32(simde_mm_castps_si128(
      Estimate(simde_mm_castsi128_ps(simde_mm_set1_epi32(static_cast<i32>(bits)))))));
}

/**
 * Reports whether an operand is a denormal other than zero.
 *
 * @param bits The operand bits.
 * @return True for a nonzero magnitude below the smallest normal.
 */
inline bool IsDenormal(u32 bits) {
  return (bits & kInfinity32) == 0 && (bits & 0x7FFFFFu) != 0;
}

/**
 * Rewrites a denormal operand as the normal operand of the same value scaled by 2^64.
 *
 * @param bits The denormal operand bits.
 * @return The scaled normal operand bits.
 */
inline u32 ScaleDenormal(u32 bits) {
  const u32 mantissa = bits & 0x7FFFFFu;
  const i32 shift = std::countl_zero(mantissa) - 8;
  const u32 biased = static_cast<u32>(1 - shift + 64);
  return (bits & kSign32) | (biased << 23) | ((mantissa << shift) & 0x7FFFFFu);
}

/**
 * Applies a scalar rule to the lanes of a vector that the fast estimate cannot handle.
 *
 * @tparam Rule The scalar rule, taking an operand and giving the result bits.
 * @tparam Needs Reports whether a lane takes the rule.
 * @param v The operand.
 * @param fast The result of the fast estimate.
 * @return The results with the special lanes replaced.
 */
template <u32 (*Rule)(u32), bool (*Needs)(u32)>
inline simde__m128 ReplaceLanes(simde__m128 v, simde__m128 fast) {
  alignas(16) std::array<u32, 4> in;
  alignas(16) std::array<u32, 4> out;
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(in.data()), simde_mm_castps_si128(v));
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(out.data()), simde_mm_castps_si128(fast));
  for (u32 lane = 0; lane < 4; ++lane) {
    if (Needs(in[lane])) {
      out[lane] = Rule(in[lane]);
    }
  }
  return simde_mm_castsi128_ps(
      simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(out.data())));
}

inline bool RefpNeeds(u32 bits) {
  const u32 biased = (bits >> 23) & 0xFFu;
  return IsDenormal(bits) || biased == 253 || biased == 254;
}

inline u32 RefpRule(u32 bits) {
  const u32 sign = bits & kSign32;
  const u32 biased = (bits >> 23) & 0xFFu;
  if (IsDenormal(bits)) {
    const u32 estimate = EstimateOne<Vrefp>(ScaleDenormal(bits));
    const u32 field = ((estimate >> 23) & 0xFFu) + 64;
    return field > 254 ? (sign | kInfinity32) : (sign | (field << 23) | (estimate & 0x7FFFFFu));
  }
  if (biased == 253 && (bits & 0x7FFFFFu) == 0) {
    return sign | 0x00800000u;
  }
  const u32 estimate = EstimateOne<Vrefp>((bits & 0x807FFFFFu) | (252u << 23));
  const u32 shift = biased - 252;
  const u32 full = 0x800000u | (estimate & 0x7FFFFFu);
  const u32 dropped = full & ((1u << shift) - 1);
  const u32 half = 1u << (shift - 1);
  const u32 kept = full >> shift;
  return sign | (kept + ((dropped > half || (dropped == half && (kept & 1))) ? 1u : 0u));
}

inline u32 RsqrtefpRule(u32 bits) {
  const u32 estimate = EstimateOne<Vrsqrtefp>(ScaleDenormal(bits));
  if ((estimate & kInfinity32) == kInfinity32) {
    return estimate;
  }
  return estimate + (32u << 23);
}

inline u32 LogefpRule(u32 bits) {
  const u32 estimate = EstimateOne<Vlogefp>(ScaleDenormal(bits));
  if ((estimate & kInfinity32) == kInfinity32) {
    return estimate;
  }
  return std::bit_cast<u32>(std::bit_cast<f32>(estimate) - 64.0f);
}

}  // namespace detail

/**
 * Computes vrefp and vrefp128 for a fixed VSCR NJ bit.
 *
 * @tparam NonJava The VSCR NJ bit.
 * @param v The operand.
 * @return The estimate of each lane.
 */
template <bool NonJava>
[[nodiscard]] inline simde__m128 Vrefp(simde__m128 v) noexcept {
  const auto fast = Vrefp(v);
  if constexpr (NonJava) {
    return fast;
  } else {
    return detail::ReplaceLanes<detail::RefpRule, detail::RefpNeeds>(v, fast);
  }
}

/**
 * Computes vrefp and vrefp128, honoring VSCR NJ.
 *
 * With NJ clear a denormal operand is estimated at its value and a denormal result is kept, rounded
 * to nearest even.
 *
 * @param v The operand.
 * @param non_java The VSCR NJ bit.
 * @return The reciprocal estimate of each lane.
 */
[[nodiscard]] inline simde__m128 Vrefp(simde__m128 v, bool non_java) noexcept {
  return non_java ? Vrefp<true>(v) : Vrefp<false>(v);
}

/**
 * Computes vrsqrtefp and vrsqrtefp128 for a fixed VSCR NJ bit.
 *
 * @tparam NonJava The VSCR NJ bit.
 * @param v The operand.
 * @return The estimate of each lane.
 */
template <bool NonJava>
[[nodiscard]] inline simde__m128 Vrsqrtefp(simde__m128 v) noexcept {
  const auto fast = Vrsqrtefp(v);
  if constexpr (NonJava) {
    return fast;
  } else {
    return detail::ReplaceLanes<detail::RsqrtefpRule, detail::IsDenormal>(v, fast);
  }
}

/**
 * Computes vrsqrtefp and vrsqrtefp128, honoring VSCR NJ.
 *
 * With NJ clear a denormal operand is estimated at its value.
 *
 * @param v The operand.
 * @param non_java The VSCR NJ bit.
 * @return The reciprocal square root estimate of each lane.
 */
[[nodiscard]] inline simde__m128 Vrsqrtefp(simde__m128 v, bool non_java) noexcept {
  return non_java ? Vrsqrtefp<true>(v) : Vrsqrtefp<false>(v);
}

/**
 * Computes vlogefp and vlogefp128 for a fixed VSCR NJ bit.
 *
 * @tparam NonJava The VSCR NJ bit.
 * @param v The operand.
 * @return The estimate of each lane.
 */
template <bool NonJava>
[[nodiscard]] inline simde__m128 Vlogefp(simde__m128 v) noexcept {
  const auto fast = Vlogefp(v);
  if constexpr (NonJava) {
    return fast;
  } else {
    return detail::ReplaceLanes<detail::LogefpRule, detail::IsDenormal>(v, fast);
  }
}

/**
 * Computes vlogefp and vlogefp128, honoring VSCR NJ.
 *
 * With NJ clear a denormal operand is estimated at its value.
 *
 * @param v The operand.
 * @param non_java The VSCR NJ bit.
 * @return The log2 estimate of each lane.
 */
[[nodiscard]] inline simde__m128 Vlogefp(simde__m128 v, bool non_java) noexcept {
  return non_java ? Vlogefp<true>(v) : Vlogefp<false>(v);
}

/**
 * Computes vexptefp and vexptefp128, honoring VSCR NJ. A denormal operand gives 1.0 whatever the
 * NJ bit holds.
 *
 * @param v The operand.
 * @param non_java The VSCR NJ bit.
 * @return The exp2 estimate of each lane.
 */
[[nodiscard]] inline simde__m128 Vexptefp(simde__m128 v, bool non_java) noexcept {
  static_cast<void>(non_java);
  return Vexptefp(v);
}

}  // namespace rex::ppc
