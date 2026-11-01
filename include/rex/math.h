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

namespace rex {

template <typename T, size_t N>
constexpr size_t CountOf(T (&)[N]) noexcept {
  return std::extent<T[N]>::value;
}

/**
 * Rounds up to a power of 2 alignment. 0 stays 0.
 *
 * @param alignment Power of 2 alignment.
 */
template <typename T>
constexpr T RoundUpPow2(T value, T alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

/**
 * Rounds up to any multiple. 0 stays 0, as with RoundUpPow2.
 *
 * @param multiple The multiple to round to, any positive value.
 */
template <typename T, typename V>
constexpr T RoundUp(T value, V multiple) {
  return (value + multiple - 1) / multiple * multiple;
}

/**
 * Rounds up to any multiple. 0 becomes one multiple, so a size of 0 takes space.
 *
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
 * @tparam T Floating point type. The lower clamp narrows through f32.
 */
template <typename T>
T clamp_float(T value, T min_value, T max_value) {
  f32 clamped_to_min = std::isgreater(value, min_value) ? value : min_value;
  return std::isless(clamped_to_min, max_value) ? clamped_to_min : max_value;
}

/** Clamps to [0, 1] with shading language rules. NaN becomes +0. */
template <typename T>
T saturate(T value) {
  return clamp_float(value, static_cast<T>(0.0f), static_cast<T>(1.0f));
}

/**
 * Divides a fraction by the greatest common divisor of its terms.
 *
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
 * @param fraction The numerator and denominator, reduced in place.
 */
template <typename T>
constexpr void reduce_fraction(std::pair<T, T>& fraction) {
  reduce_fraction<T>(fraction.first, fraction.second);
}

/**
 * Multiplies a value by a fraction using a 128 bit product.
 *
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
 * Saturating addition, from https://locklessinc.com/articles/sat_arithmetic/
 *
 * @return a + b clamped to the range of T.
 */
template <typename T>
inline T SatAdd(T a, T b) {
  using TU = std::make_unsigned<T>::type;
  TU result = static_cast<TU>(a) + static_cast<TU>(b);
  if (std::is_unsigned<T>::value) {
    result |= static_cast<TU>(-static_cast<std::make_signed<T>::type>(result < static_cast<TU>(a)));
  } else {
    TU overflowed = (static_cast<TU>(a) >> (sizeof(T) * 8 - 1)) + std::numeric_limits<T>::max();
    if (static_cast<T>((overflowed ^ static_cast<TU>(b)) | ~(static_cast<TU>(b) ^ result)) >= 0) {
      result = overflowed;
    }
  }
  return static_cast<T>(result);
}

/**
 * Saturating subtraction, from https://locklessinc.com/articles/sat_arithmetic/
 *
 * @return a - b clamped to the range of T.
 */
template <typename T>
inline T SatSub(T a, T b) {
  using TU = std::make_unsigned<T>::type;
  TU result = static_cast<TU>(a) - static_cast<TU>(b);
  if (std::is_unsigned<T>::value) {
    result &=
        static_cast<TU>(-static_cast<std::make_signed<T>::type>(result <= static_cast<TU>(a)));
  } else {
    TU overflowed = (static_cast<TU>(a) >> (sizeof(T) * 8 - 1)) + std::numeric_limits<T>::max();
    if (static_cast<T>((overflowed ^ static_cast<TU>(b)) & (overflowed ^ result)) < 0) {
      result = overflowed;
    }
  }
  return static_cast<T>(result);
}

/** A two component size or offset in pixels. */
struct Vec2 {
  f32 x = 0;
  f32 y = 0;

  constexpr bool operator==(const Vec2& other) const = default;
};

}  // namespace rex
