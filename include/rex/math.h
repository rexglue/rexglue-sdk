/**
 * @file        math.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/math.h (Ben Vanik, 2019).
 */

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <type_traits>
#include <utility>

#include <rex/assert.h>
#include <rex/platform.h>
#include <rex/types.h>

#include <simde/x86/sse2.h>

namespace rex {

/**
 * @tparam T Element type.
 * @tparam N Element count.
 * @return The number of elements in a C array.
 */
template <typename T, size_t N>
constexpr size_t CountOf(T (&)[N]) noexcept {
  return std::extent<T[N]>::value;
}

/**
 * @tparam T Integer type.
 * @param value The value to test.
 * @return True when value is a power of 2 or 0.
 */
template <typename T>
constexpr bool is_pow2(T value) {
  return (value & (value - 1)) == 0;
}

/**
 * Rounds up to a power of 2 alignment. 0 stays 0.
 *
 * @tparam T Integer type.
 * @param value The value to round up.
 * @param alignment Power of 2 alignment.
 * @return value rounded up to a multiple of alignment.
 */
template <typename T>
constexpr T RoundUpPow2(T value, T alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

/**
 * Rounds up to any multiple. 0 stays 0, as with RoundUpPow2.
 *
 * @tparam T Value type.
 * @tparam V Multiple type.
 * @param value The value to round up.
 * @param multiple The multiple to round to, any positive value.
 * @return value rounded up to a multiple of multiple.
 */
template <typename T, typename V>
constexpr T RoundUp(T value, V multiple) {
  return (value + multiple - 1) / multiple * multiple;
}

/**
 * Rounds up to any multiple. 0 becomes 1 multiple, so a size of 0 still takes space.
 *
 * @tparam T Value type.
 * @tparam V Multiple type.
 * @param value The value to round up.
 * @param multiple The multiple to round to, any positive value.
 * @return value rounded up to a multiple of multiple, never 0.
 */
template <typename T, typename V>
constexpr T RoundUpNonZero(T value, V multiple) {
  if (!value) {
    return multiple;
  }
  return RoundUp(value, multiple);
}

/**
 * Clamps a floating point value. NaN becomes min_value, or max_value when min_value is NaN too.
 * A result at a zero bound takes the sign of that bound.
 *
 * @tparam T Floating point type.
 * @param value The value to clamp.
 * @param min_value The lower bound.
 * @param max_value The upper bound.
 * @return The clamped value.
 */
template <typename T>
T clamp_float(T value, T min_value, T max_value) {
  f32 clamped_to_min = std::isgreater(value, min_value) ? value : min_value;
  return std::isless(clamped_to_min, max_value) ? clamped_to_min : max_value;
}

/**
 * Clamps to [0, 1] with shading language rules. NaN becomes +0.
 *
 * @tparam T Floating point type.
 * @param value The value to clamp.
 * @return The clamped value.
 */
template <typename T>
T saturate(T value) {
  return clamp_float(value, static_cast<T>(0.0f), static_cast<T>(1.0f));
}

/**
 * @tparam T Unsigned integer type up to 32 bits.
 * @param value The value to round up.
 * @return The smallest power of 2 greater than or equal to value.
 */
template <typename T>
T next_pow2(T value) {
  value--;
  value |= value >> 1;
  value |= value >> 2;
  value |= value >> 4;
  value |= value >> 8;
  value |= value >> 16;
  value++;
  return value;
}

/**
 * Divides a fraction by the greatest common divisor of its terms.
 *
 * @tparam T Integer type.
 * @param numerator The numerator, reduced in place.
 * @param denominator The denominator, reduced in place.
 */
template <typename T>
constexpr void reduce_fraction(T& numerator, T& denominator) {
  auto gcd = std::gcd(numerator, denominator);
  numerator /= gcd;
  denominator /= gcd;
}

/**
 * Divides a fraction by the greatest common divisor of its terms.
 *
 * @tparam T Integer type.
 * @param fraction The numerator and denominator, reduced in place.
 */
template <typename T>
constexpr void reduce_fraction(std::pair<T, T>& fraction) {
  reduce_fraction<T>(fraction.first, fraction.second);
}

/**
 * Multiplies a value by a fraction through a 128-bit product and a 128 by 64 bit division.
 *
 * @param value The value to scale.
 * @param numerator The fraction numerator.
 * @param denominator The fraction denominator, not 0.
 * @return The floor of value * numerator / denominator, saturated at the u64 maximum. A 0
 *         denominator also returns the u64 maximum.
 */
constexpr u64 mul_div(u64 value, u64 numerator, u64 denominator) {
  REX_ASSERT(denominator != 0);
  constexpr u64 kMax = std::numeric_limits<u64>::max();
  constexpr u64 kHalfMask = 0xFFFFFFFF;
  if (denominator == 0) {
    return kMax;
  }
  const unsigned __int128 product = static_cast<unsigned __int128>(value) * numerator;
  u64 high = static_cast<u64>(product >> 64);
  u64 low = static_cast<u64>(product);
  if (high >= denominator) {
    return kMax;
  }
  if (high == 0) {
    return low / denominator;
  }

  const int shift = std::countl_zero(denominator);
  denominator <<= shift;
  high = (high << shift) | (shift == 0 ? 0 : low >> (64 - shift));
  low <<= shift;
  const u64 divisor_high = denominator >> 32;
  const u64 divisor_low = denominator & kHalfMask;

  const auto divide_step = [&](u64 dividend, u64 next_half) {
    u64 quotient = dividend / divisor_high;
    u64 remainder = dividend % divisor_high;
    while ((quotient >> 32) != 0 || quotient * divisor_low > ((remainder << 32) | next_half)) {
      --quotient;
      remainder += divisor_high;
      if ((remainder >> 32) != 0) {
        break;
      }
    }
    return quotient;
  };

  const u64 low_high = low >> 32;
  const u64 quotient_high = divide_step(high, low_high);
  const u64 partial = ((high << 32) | low_high) - quotient_high * denominator;
  const u64 quotient_low = divide_step(partial, low & kHalfMask);
  return (quotient_high << 32) | quotient_low;
}

/**
 * @param a Index of the lowest set bit.
 * @param b Index of the highest set bit.
 * @return A mask with bits a through b set.
 */
constexpr u32 MakeBitmask(u32 a, u32 b) {
  return (static_cast<u32>(-1) >> (31 - b)) & ~((1u << a) - 1);
}

/**
 * @param value The source value.
 * @param a Index of the lowest bit to extract.
 * @param b Index of the highest bit to extract.
 * @return Bits a through b of value, shifted down to bit 0.
 */
constexpr u32 select_bits(u32 value, u32 a, u32 b) {
  return (value & MakeBitmask(a, b)) >> a;
}

/**
 * @tparam T Unsigned integer type.
 * @param v The value to count.
 * @return The number of set bits in v.
 */
template <class T>
constexpr u32 BitCount(T v) {
  return static_cast<u32>(std::popcount(v));
}

/**
 * @param v The value to scan.
 * @return The number of leading zero bits, or 8 when v is 0.
 */
inline u8 lzcnt(u8 v) {
  return static_cast<u8>(std::countl_zero(v));
}

/**
 * @param v The value to scan.
 * @return The number of leading zero bits, or 16 when v is 0.
 */
inline u8 lzcnt(u16 v) {
  return static_cast<u8>(std::countl_zero(v));
}

/**
 * @param v The value to scan.
 * @return The number of leading zero bits, or 32 when v is 0.
 */
inline u8 lzcnt(u32 v) {
  return static_cast<u8>(std::countl_zero(v));
}

/**
 * @param v The value to scan.
 * @return The number of leading zero bits, or 64 when v is 0.
 */
inline u8 lzcnt(u64 v) {
  return static_cast<u8>(std::countl_zero(v));
}

/**
 * @param v The value to scan.
 * @return The number of trailing zero bits, or 8 when v is 0.
 */
inline u8 tzcnt(u8 v) {
  return static_cast<u8>(std::countr_zero(v));
}

/**
 * @param v The value to scan.
 * @return The number of trailing zero bits, or 16 when v is 0.
 */
inline u8 tzcnt(u16 v) {
  return static_cast<u8>(std::countr_zero(v));
}

/**
 * @param v The value to scan.
 * @return The number of trailing zero bits, or 32 when v is 0.
 */
inline u8 tzcnt(u32 v) {
  return static_cast<u8>(std::countr_zero(v));
}

/**
 * @param v The value to scan.
 * @return The number of trailing zero bits, or 64 when v is 0.
 */
inline u8 tzcnt(u64 v) {
  return static_cast<u8>(std::countr_zero(v));
}

/**
 * @param v The value to scan, read as unsigned.
 * @return The number of leading zero bits, or 8 when v is 0.
 */
inline u8 lzcnt(i8 v) {
  return lzcnt(static_cast<u8>(v));
}

/**
 * @param v The value to scan, read as unsigned.
 * @return The number of leading zero bits, or 16 when v is 0.
 */
inline u8 lzcnt(i16 v) {
  return lzcnt(static_cast<u16>(v));
}

/**
 * @param v The value to scan, read as unsigned.
 * @return The number of leading zero bits, or 32 when v is 0.
 */
inline u8 lzcnt(i32 v) {
  return lzcnt(static_cast<u32>(v));
}

/**
 * @param v The value to scan, read as unsigned.
 * @return The number of leading zero bits, or 64 when v is 0.
 */
inline u8 lzcnt(i64 v) {
  return lzcnt(static_cast<u64>(v));
}

/**
 * @param v The value to scan, read as unsigned.
 * @return The number of trailing zero bits, or 8 when v is 0.
 */
inline u8 tzcnt(i8 v) {
  return tzcnt(static_cast<u8>(v));
}

/**
 * @param v The value to scan, read as unsigned.
 * @return The number of trailing zero bits, or 16 when v is 0.
 */
inline u8 tzcnt(i16 v) {
  return tzcnt(static_cast<u16>(v));
}

/**
 * @param v The value to scan, read as unsigned.
 * @return The number of trailing zero bits, or 32 when v is 0.
 */
inline u8 tzcnt(i32 v) {
  return tzcnt(static_cast<u32>(v));
}

/**
 * @param v The value to scan, read as unsigned.
 * @return The number of trailing zero bits, or 64 when v is 0.
 */
inline u8 tzcnt(i64 v) {
  return tzcnt(static_cast<u64>(v));
}

/**
 * Finds the lowest set bit, like the x86 BSF instruction.
 *
 * @param v The value to scan.
 * @param out_first_set_index Receives the index of the lowest set bit.
 * @return False when v is 0, leaving out_first_set_index invalid.
 */
inline bool bit_scan_forward(u32 v, u32* out_first_set_index) {
  if (v == 0) {
    return false;
  }
  *out_first_set_index = static_cast<u32>(std::countr_zero(v));
  return true;
}

/**
 * Finds the lowest set bit, like the x86 BSF instruction.
 *
 * @param v The value to scan.
 * @param out_first_set_index Receives the index of the lowest set bit.
 * @return False when v is 0, leaving out_first_set_index invalid.
 */
inline bool bit_scan_forward(u64 v, u32* out_first_set_index) {
  if (v == 0) {
    return false;
  }
  *out_first_set_index = static_cast<u32>(std::countr_zero(v));
  return true;
}

/**
 * Finds the lowest set bit, like the x86 BSF instruction.
 *
 * @param v The value to scan, read as unsigned.
 * @param out_first_set_index Receives the index of the lowest set bit.
 * @return False when v is 0, leaving out_first_set_index invalid.
 */
inline bool bit_scan_forward(i32 v, u32* out_first_set_index) {
  return bit_scan_forward(static_cast<u32>(v), out_first_set_index);
}

/**
 * Finds the lowest set bit, like the x86 BSF instruction.
 *
 * @param v The value to scan, read as unsigned.
 * @param out_first_set_index Receives the index of the lowest set bit.
 * @return False when v is 0, leaving out_first_set_index invalid.
 */
inline bool bit_scan_forward(i64 v, u32* out_first_set_index) {
  return bit_scan_forward(static_cast<u64>(v), out_first_set_index);
}

/**
 * @tparam T Integer type.
 * @param v A value greater than 0.
 * @return The base 2 logarithm of v rounded down.
 */
template <typename T>
inline T log2_floor(T v) {
  return sizeof(T) * 8 - 1 - lzcnt(v);
}

/**
 * @tparam T Integer type.
 * @param v A value greater than 0.
 * @return The base 2 logarithm of v rounded up.
 */
template <typename T>
inline T log2_ceil(T v) {
  return sizeof(T) * 8 - lzcnt(v - 1);
}

/**
 * @tparam T Unsigned integer type.
 * @param v The value to rotate.
 * @param sh Rotation amount in bits, between 1 and the bit width of T minus 1.
 * @return v rotated left by sh bits.
 */
template <typename T>
inline T RotateLeft(T v, u8 sh) {
  return (T(v) << sh) | (T(v) >> ((sizeof(T) * 8) - sh));
}

/**
 * @tparam N Lane index, 0 to 3.
 * @param v The SSE register.
 * @return Lane N of v as f32.
 */
template <int N>
f32 m128_f32(const simde__m128& v) {
  f32 ret;
  simde_mm_store_ss(&ret, simde_mm_shuffle_ps(v, v, SIMDE_MM_SHUFFLE(N, N, N, N)));
  return ret;
}

/**
 * @tparam N Lane index, 0 to 3.
 * @param v The SSE register.
 * @return The bits of lane N of v as i32.
 */
template <int N>
i32 m128_i32(const simde__m128& v) {
  f32 f;
  simde_mm_store_ss(&f, simde_mm_shuffle_ps(v, v, SIMDE_MM_SHUFFLE(N, N, N, N)));
  return std::bit_cast<i32>(f);
}

/**
 * @tparam N Lane index, 0 or 1.
 * @param v The SSE register.
 * @return Lane N of v as f64.
 */
template <int N>
f64 m128_f64(const simde__m128d& v) {
  f64 ret;
  simde_mm_store_sd(&ret, simde_mm_shuffle_pd(v, v, SIMDE_MM_SHUFFLE2(N, N)));
  return ret;
}

/**
 * @tparam N Lane index, 0 or 1.
 * @param v The SSE register, read as 2 f64 lanes.
 * @return Lane N of v as f64.
 */
template <int N>
f64 m128_f64(const simde__m128& v) {
  return m128_f64<N>(simde_mm_castps_pd(v));
}

/**
 * @tparam N Lane index, 0 or 1.
 * @param v The SSE register.
 * @return The bits of lane N of v as i64.
 */
template <int N>
i64 m128_i64(const simde__m128d& v) {
  f64 f;
  simde_mm_store_sd(&f, simde_mm_shuffle_pd(v, v, SIMDE_MM_SHUFFLE2(N, N)));
  return std::bit_cast<i64>(f);
}

/**
 * @tparam N Lane index, 0 or 1.
 * @param v The SSE register, read as 2 64 bit lanes.
 * @return The bits of lane N of v as i64.
 */
template <int N>
i64 m128_i64(const simde__m128& v) {
  return m128_i64<N>(simde_mm_castps_pd(v));
}

/**
 * Saturating addition, from https://locklessinc.com/articles/sat_arithmetic/
 *
 * @tparam T Integer type.
 * @param a First operand.
 * @param b Second operand.
 * @return a + b clamped to the range of T.
 */
template <typename T>
inline T SatAdd(T a, T b) {
  using TU = std::make_unsigned<T>::type;
  TU result = TU(a) + TU(b);
  if (std::is_unsigned<T>::value) {
    result |= TU(-static_cast<std::make_signed<T>::type>(result < TU(a)));
  } else {
    TU overflowed = (TU(a) >> (sizeof(T) * 8 - 1)) + std::numeric_limits<T>::max();
    if (T((overflowed ^ TU(b)) | ~(TU(b) ^ result)) >= 0) {
      result = overflowed;
    }
  }
  return T(result);
}

/**
 * Saturating subtraction, from https://locklessinc.com/articles/sat_arithmetic/
 *
 * @tparam T Integer type.
 * @param a First operand.
 * @param b Second operand.
 * @return a - b clamped to the range of T.
 */
template <typename T>
inline T SatSub(T a, T b) {
  using TU = std::make_unsigned<T>::type;
  TU result = TU(a) - TU(b);
  if (std::is_unsigned<T>::value) {
    result &= TU(-static_cast<std::make_signed<T>::type>(result <= TU(a)));
  } else {
    TU overflowed = (TU(a) >> (sizeof(T) * 8 - 1)) + std::numeric_limits<T>::max();
    if (T((overflowed ^ TU(b)) & (overflowed ^ result)) < 0) {
      result = overflowed;
    }
  }
  return T(result);
}

}  // namespace rex
