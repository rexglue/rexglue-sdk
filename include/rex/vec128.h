/**
 * @file        vec128.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/vec128.h (Ben Vanik, 2013).
 */

#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>

#include <rex/math.h>
#include <rex/platform.h>
#include <rex/types.h>

namespace rex {

/**
 * 128 bit vector register value.
 *
 * Lanes are stored in AVX little endian order, the reverse of the Altivec big endian order.
 * Big endian element i is stored at u32[i ^ 0x3], u16[i ^ 0x7] and u8[i ^ 0xF]. Code should avoid
 * fixed lane offsets where a loop over the lanes works.
 */
struct alignas(16) vec128_s {
  union {
    struct {
      rex::f32 x; /**< f32 lane 0. */
      rex::f32 y; /**< f32 lane 1. */
      rex::f32 z; /**< f32 lane 2. */
      rex::f32 w; /**< f32 lane 3. */
    };
    struct {
      rex::i32 ix; /**< i32 lane 0. */
      rex::i32 iy; /**< i32 lane 1. */
      rex::i32 iz; /**< i32 lane 2. */
      rex::i32 iw; /**< i32 lane 3. */
    };
    struct {
      rex::u32 ux; /**< u32 lane 0. */
      rex::u32 uy; /**< u32 lane 1. */
      rex::u32 uz; /**< u32 lane 2. */
      rex::u32 uw; /**< u32 lane 3. */
    };
    rex::f32 f32[4]; /**< 4 f32 lanes. */
    rex::f64 f64[2]; /**< 2 f64 lanes. */
    rex::i8 i8[16];  /**< 16 i8 lanes. */
    rex::u8 u8[16];  /**< 16 u8 lanes. */
    rex::i16 i16[8]; /**< 8 i16 lanes. */
    rex::u16 u16[8]; /**< 8 u16 lanes. */
    rex::i32 i32[4]; /**< 4 i32 lanes. */
    rex::u32 u32[4]; /**< 4 u32 lanes. */
    rex::i64 i64[2]; /**< 2 i64 lanes. */
    rex::u64 u64[2]; /**< 2 u64 lanes. */
    struct {
      rex::u64 low;  /**< Low 64 bits. */
      rex::u64 high; /**< High 64 bits. */
    };
  };

  /**
   * @param b The value to compare with.
   * @return True when all 128 bits match.
   */
  bool operator==(const vec128_s& b) const { return low == b.low && high == b.high; }

  /**
   * @param b The value to compare with.
   * @return True when any bit differs.
   */
  bool operator!=(const vec128_s& b) const { return low != b.low || high != b.high; }

  /**
   * @param b The right operand.
   * @return The bitwise XOR of this and b.
   */
  vec128_s operator^(const vec128_s& b) const {
    vec128_s a = *this;
    a.high ^= b.high;
    a.low ^= b.low;
    return a;
  };

  /**
   * @param b The right operand.
   * @return This value after the bitwise XOR with b.
   */
  vec128_s& operator^=(const vec128_s& b) {
    *this = *this ^ b;
    return *this;
  };

  /**
   * @param b The right operand.
   * @return The bitwise AND of this and b.
   */
  vec128_s operator&(const vec128_s& b) const {
    vec128_s a = *this;
    a.high &= b.high;
    a.low &= b.low;
    return a;
  };

  /**
   * @param b The right operand.
   * @return This value after the bitwise AND with b.
   */
  vec128_s& operator&=(const vec128_s& b) {
    *this = *this & b;
    return *this;
  };

  /**
   * @param b The right operand.
   * @return The bitwise OR of this and b.
   */
  vec128_s operator|(const vec128_s& b) const {
    vec128_s a = *this;
    a.high |= b.high;
    a.low |= b.low;
    return a;
  };

  /**
   * @param b The right operand.
   * @return This value after the bitwise OR with b.
   */
  vec128_s& operator|=(const vec128_s& b) {
    *this = *this | b;
    return *this;
  };
};

/** 128 bit vector register value. */
using vec128_t = vec128_s;

/**
 * @param src The value for every u32 lane.
 * @return A vector with src in all 4 u32 lanes.
 */
static inline vec128_t vec128i(u32 src) {
  vec128_t v;
  for (auto i = 0; i < 4; ++i) {
    v.u32[i] = src;
  }
  return v;
}

/**
 * @param x u32 lane 0.
 * @param y u32 lane 1.
 * @param z u32 lane 2.
 * @param w u32 lane 3.
 * @return A vector with the given u32 lanes.
 */
static inline vec128_t vec128i(u32 x, u32 y, u32 z, u32 w) {
  vec128_t v;
  v.u32[0] = x;
  v.u32[1] = y;
  v.u32[2] = z;
  v.u32[3] = w;
  return v;
}

/**
 * @param src The value for every 64 bit lane.
 * @return A vector with src in both 64 bit lanes.
 */
static inline vec128_t vec128q(u64 src) {
  vec128_t v;
  for (auto i = 0; i < 2; ++i) {
    v.i64[i] = src;
  }
  return v;
}

/**
 * @param x 64 bit lane 0.
 * @param y 64 bit lane 1.
 * @return A vector with the given 64 bit lanes.
 */
static inline vec128_t vec128q(u64 x, u64 y) {
  vec128_t v;
  v.i64[0] = x;
  v.i64[1] = y;
  return v;
}

/**
 * @param src The value for every f64 lane.
 * @return A vector with src in both f64 lanes.
 */
static inline vec128_t vec128d(f64 src) {
  vec128_t v;
  for (auto i = 0; i < 2; ++i) {
    v.f64[i] = src;
  }
  return v;
}

/**
 * @param x f64 lane 0.
 * @param y f64 lane 1.
 * @return A vector with the given f64 lanes.
 */
static inline vec128_t vec128d(f64 x, f64 y) {
  vec128_t v;
  v.f64[0] = x;
  v.f64[1] = y;
  return v;
}

/**
 * @param src The value for every f32 lane.
 * @return A vector with src in all 4 f32 lanes.
 */
static inline vec128_t vec128f(f32 src) {
  vec128_t v;
  for (auto i = 0; i < 4; ++i) {
    v.f32[i] = src;
  }
  return v;
}

/**
 * @param x f32 lane 0.
 * @param y f32 lane 1.
 * @param z f32 lane 2.
 * @param w f32 lane 3.
 * @return A vector with the given f32 lanes.
 */
static inline vec128_t vec128f(f32 x, f32 y, f32 z, f32 w) {
  vec128_t v;
  v.f32[0] = x;
  v.f32[1] = y;
  v.f32[2] = z;
  v.f32[3] = w;
  return v;
}

/**
 * @param src The value for every u16 lane.
 * @return A vector with src in all 8 u16 lanes.
 */
static inline vec128_t vec128s(u16 src) {
  vec128_t v;
  for (auto i = 0; i < 8; ++i) {
    v.u16[i] = src;
  }
  return v;
}

/**
 * Builds a vector from u16 values given in big endian order within each 32 bit lane.
 *
 * @param x0 High half of 32 bit lane 0.
 * @param x1 Low half of 32 bit lane 0.
 * @param y0 High half of 32 bit lane 1.
 * @param y1 Low half of 32 bit lane 1.
 * @param z0 High half of 32 bit lane 2.
 * @param z1 Low half of 32 bit lane 2.
 * @param w0 High half of 32 bit lane 3.
 * @param w1 Low half of 32 bit lane 3.
 * @return A vector with the given u16 values.
 */
static inline vec128_t vec128s(u16 x0, u16 x1, u16 y0, u16 y1, u16 z0, u16 z1, u16 w0, u16 w1) {
  vec128_t v;
  v.u16[0] = x1;
  v.u16[1] = x0;
  v.u16[2] = y1;
  v.u16[3] = y0;
  v.u16[4] = z1;
  v.u16[5] = z0;
  v.u16[6] = w1;
  v.u16[7] = w0;
  return v;
}

/**
 * @param src The value for every u8 lane.
 * @return A vector with src in all 16 u8 lanes.
 */
static inline vec128_t vec128b(u8 src) {
  vec128_t v;
  for (auto i = 0; i < 16; ++i) {
    v.u8[i] = src;
  }
  return v;
}

/**
 * Builds a vector from u8 values given in big endian order within each 32 bit lane.
 *
 * @param x0 Byte 0, the most significant, of 32 bit lane 0.
 * @param x1 Byte 1 of 32 bit lane 0.
 * @param x2 Byte 2 of 32 bit lane 0.
 * @param x3 Byte 3, the least significant, of 32 bit lane 0.
 * @param y0 Byte 0 of 32 bit lane 1.
 * @param y1 Byte 1 of 32 bit lane 1.
 * @param y2 Byte 2 of 32 bit lane 1.
 * @param y3 Byte 3 of 32 bit lane 1.
 * @param z0 Byte 0 of 32 bit lane 2.
 * @param z1 Byte 1 of 32 bit lane 2.
 * @param z2 Byte 2 of 32 bit lane 2.
 * @param z3 Byte 3 of 32 bit lane 2.
 * @param w0 Byte 0 of 32 bit lane 3.
 * @param w1 Byte 1 of 32 bit lane 3.
 * @param w2 Byte 2 of 32 bit lane 3.
 * @param w3 Byte 3 of 32 bit lane 3.
 * @return A vector with the given u8 values.
 */
static inline vec128_t vec128b(u8 x0, u8 x1, u8 x2, u8 x3, u8 y0, u8 y1, u8 y2, u8 y3, u8 z0, u8 z1,
                               u8 z2, u8 z3, u8 w0, u8 w1, u8 w2, u8 w3) {
  vec128_t v;
  v.u8[0] = x3;
  v.u8[1] = x2;
  v.u8[2] = x1;
  v.u8[3] = x0;
  v.u8[4] = y3;
  v.u8[5] = y2;
  v.u8[6] = y1;
  v.u8[7] = y0;
  v.u8[8] = z3;
  v.u8[9] = z2;
  v.u8[10] = z1;
  v.u8[11] = z0;
  v.u8[12] = w3;
  v.u8[13] = w2;
  v.u8[14] = w1;
  v.u8[15] = w0;
  return v;
}

/**
 * @param value The vector to format.
 * @return The 4 f32 lanes as "(x, y, z, w)".
 */
std::string to_string(const vec128_t& value);

/**
 * Writes the 4 u32 lanes as 8 digit uppercase hex, "[X X X X]".
 *
 * @param os The stream to write to.
 * @param value The vector to write.
 * @return os.
 */
std::ostream& operator<<(std::ostream& os, const vec128_t& value);

}  // namespace rex
