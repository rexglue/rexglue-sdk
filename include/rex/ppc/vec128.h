/**
 * @file        ppc/vec128.h
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

#include <charconv>
#include <cstddef>
#include <format>
#include <iosfwd>
#include <string>
#include <string_view>

#include <rex/assert.h>
#include <rex/platform.h>
#include <rex/string/numeric.h>
#include <rex/types.h>

#include <simde/x86/sse2.h>

namespace rex::ppc {

/**
 * 128 bit vector register value.
 *
 * Big endian element i is stored at u32[i ^ 0x3], u16[i ^ 0x7] and u8[i ^ 0xF].
 */
struct alignas(16) Vec128S {
  union {
    struct {
      rex::f32 x;
      rex::f32 y;
      rex::f32 z;
      rex::f32 w;
    };
    struct {
      rex::i32 ix;
      rex::i32 iy;
      rex::i32 iz;
      rex::i32 iw;
    };
    struct {
      rex::u32 ux;
      rex::u32 uy;
      rex::u32 uz;
      rex::u32 uw;
    };
    rex::f32 f32[4];
    rex::f64 f64[2];
    rex::i8 i8[16];
    rex::u8 u8[16];
    rex::i16 i16[8];
    rex::u16 u16[8];
    rex::i32 i32[4];
    rex::u32 u32[4];
    rex::i64 i64[2];
    rex::u64 u64[2];
    struct {
      rex::u64 low;
      rex::u64 high;
    };
  };

  bool operator==(const Vec128S& b) const { return low == b.low && high == b.high; }

  bool operator!=(const Vec128S& b) const { return low != b.low || high != b.high; }

  Vec128S operator^(const Vec128S& b) const {
    Vec128S a = *this;
    a.high ^= b.high;
    a.low ^= b.low;
    return a;
  };

  /**
   * Applies a bitwise XOR with b to this value.
   *
   * @return This value.
   */
  Vec128S& operator^=(const Vec128S& b) {
    *this = *this ^ b;
    return *this;
  };

  Vec128S operator&(const Vec128S& b) const {
    Vec128S a = *this;
    a.high &= b.high;
    a.low &= b.low;
    return a;
  };

  /**
   * Applies a bitwise AND with b to this value.
   *
   * @return This value.
   */
  Vec128S& operator&=(const Vec128S& b) {
    *this = *this & b;
    return *this;
  };

  Vec128S operator|(const Vec128S& b) const {
    Vec128S a = *this;
    a.high |= b.high;
    a.low |= b.low;
    return a;
  };

  /**
   * Applies a bitwise OR with b to this value.
   *
   * @return This value.
   */
  Vec128S& operator|=(const Vec128S& b) {
    *this = *this | b;
    return *this;
  };
};

using Vec128 = Vec128S;

/** Returns a vector with the same value in all four u32 lanes. */
static inline Vec128 vec128i(u32 src) {
  Vec128 v;
  for (auto i = 0; i < 4; ++i) {
    v.u32[i] = src;
  }
  return v;
}

/**
 * Returns a vector with the given values in the four u32 lanes.
 *
 * @param x u32 lane 0.
 * @param y u32 lane 1.
 * @param z u32 lane 2.
 * @param w u32 lane 3.
 */
static inline Vec128 vec128i(u32 x, u32 y, u32 z, u32 w) {
  Vec128 v;
  v.u32[0] = x;
  v.u32[1] = y;
  v.u32[2] = z;
  v.u32[3] = w;
  return v;
}

/** Returns a vector with the same value in both 64 bit lanes. */
static inline Vec128 vec128q(u64 src) {
  Vec128 v;
  for (auto i = 0; i < 2; ++i) {
    v.u64[i] = src;
  }
  return v;
}

/**
 * Returns a vector with the given values in both 64 bit lanes.
 *
 * @param x 64 bit lane 0.
 * @param y 64 bit lane 1.
 */
static inline Vec128 vec128q(u64 x, u64 y) {
  Vec128 v;
  v.u64[0] = x;
  v.u64[1] = y;
  return v;
}

/** Returns a vector with the same value in both f64 lanes. */
static inline Vec128 vec128d(f64 src) {
  Vec128 v;
  for (auto i = 0; i < 2; ++i) {
    v.f64[i] = src;
  }
  return v;
}

/**
 * Returns a vector with the given values in the two f64 lanes.
 *
 * @param x f64 lane 0.
 * @param y f64 lane 1.
 */
static inline Vec128 vec128d(f64 x, f64 y) {
  Vec128 v;
  v.f64[0] = x;
  v.f64[1] = y;
  return v;
}

/** Returns a vector with the same value in all four f32 lanes. */
static inline Vec128 vec128f(f32 src) {
  Vec128 v;
  for (auto i = 0; i < 4; ++i) {
    v.f32[i] = src;
  }
  return v;
}

/**
 * Returns a vector with the given values in the four f32 lanes.
 *
 * @param x f32 lane 0.
 * @param y f32 lane 1.
 * @param z f32 lane 2.
 * @param w f32 lane 3.
 */
static inline Vec128 vec128f(f32 x, f32 y, f32 z, f32 w) {
  Vec128 v;
  v.f32[0] = x;
  v.f32[1] = y;
  v.f32[2] = z;
  v.f32[3] = w;
  return v;
}

/** Returns a vector with the same value in all eight u16 lanes. */
static inline Vec128 vec128s(u16 src) {
  Vec128 v;
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
static inline Vec128 vec128s(u16 x0, u16 x1, u16 y0, u16 y1, u16 z0, u16 z1, u16 w0, u16 w1) {
  Vec128 v;
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

/** Returns a vector with the same value in all 16 u8 lanes. */
static inline Vec128 vec128b(u8 src) {
  Vec128 v;
  for (auto i = 0; i < 16; ++i) {
    v.u8[i] = src;
  }
  return v;
}

/**
 * Builds a vector from u8 values given in big endian order within each 32 bit lane.
 *
 * @param x0 Most significant byte (byte 0) of 32 bit lane 0.
 * @param x1 Byte 1 of 32 bit lane 0.
 * @param x2 Byte 2 of 32 bit lane 0.
 * @param x3 Least significant byte (byte 3) of 32 bit lane 0.
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
static inline Vec128 vec128b(u8 x0, u8 x1, u8 x2, u8 x3, u8 y0, u8 y1, u8 y2, u8 y3, u8 z0, u8 z1,
                             u8 z2, u8 z3, u8 w0, u8 w1, u8 w2, u8 w3) {
  Vec128 v;
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

/** Returns the four f32 lanes as "(x, y, z, w)". */
std::string to_string(const Vec128& value);

/** Writes the four u32 lanes as 8 digit uppercase hex, "[X X X X]". */
std::ostream& operator<<(std::ostream& os, const Vec128& value);

/**
 * Formats the four u32 lanes of a vector as hex.
 *
 * @return The lanes as "[XXXXXXXX XXXXXXXX XXXXXXXX XXXXXXXX]".
 */
inline std::string to_hex_string(const Vec128& value) {
  return std::format("[{:08X} {:08X} {:08X} {:08X}]", value.u32[0], value.u32[1], value.u32[2],
                     value.u32[3]);
}

}  // namespace rex::ppc

namespace rex::string {

/**
 * Parses "[a b c d]" or bare text as four hex u32 lanes, and "(a, b, c, d)" as four f32 lanes.
 *
 * @param force_hex Unused, the opening bracket picks the format.
 * @return The vector, or a zero vector when the text does not parse.
 */
template <>
inline ppc::Vec128 from_string<ppc::Vec128>(const std::string_view value, bool force_hex) {
  if (value.empty()) {
    return ppc::Vec128();
  }
  ppc::Vec128 v;
  auto p = value.data();
  auto end = value.data() + value.size();
  bool is_hex = force_hex;
  if (p != end && *p == '[') {
    is_hex = true;
    ++p;
  } else if (p != end && *p == '(') {
    is_hex = false;
    ++p;
  } else {
    is_hex = true;
  }
  if (p == end) {
    REX_ASSERT_ALWAYS();
    return ppc::Vec128();
  }
  if (is_hex) {
    for (size_t i = 0; i < 4; i++) {
      while (p != end && (*p == ' ' || *p == ',')) {
        ++p;
      }
      if (p == end) {
        REX_ASSERT_ALWAYS();
        return ppc::Vec128();
      }
      auto result = std::from_chars(p, end, v.u32[i], 16);
      if (result.ec != std::errc()) {
        REX_ASSERT_ALWAYS();
        return ppc::Vec128();
      }
      p = result.ptr;
    }
  } else {
    for (size_t i = 0; i < 4; i++) {
      while (p != end && (*p == ' ' || *p == ',')) {
        ++p;
      }
      if (p == end) {
        REX_ASSERT_ALWAYS();
        return ppc::Vec128();
      }
#if REX_PLATFORM_MAC
      auto result = detail::portable_float_from_chars(p, end, v.f32[i]);
#else
      auto result = std::from_chars(p, end, v.f32[i], std::chars_format::general);
#endif
      if (result.ec != std::errc()) {
        REX_ASSERT_ALWAYS();
        return ppc::Vec128();
      }
      p = result.ptr;
    }
  }
  return v;
}

}  // namespace rex::string

namespace rex::ppc {

/**
 * Returns a lane of an SSE register as f32.
 *
 * @tparam N Lane index, 0 to 3.
 */
template <int N>
f32 m128_f32(const simde__m128& v) {
  f32 ret;
  simde_mm_store_ss(&ret, simde_mm_shuffle_ps(v, v, SIMDE_MM_SHUFFLE(N, N, N, N)));
  return ret;
}

/**
 * Returns the bits of a lane of an SSE register as i32.
 *
 * @tparam N Lane index, 0 to 3.
 */
template <int N>
i32 m128_i32(const simde__m128& v) {
  f32 f;
  simde_mm_store_ss(&f, simde_mm_shuffle_ps(v, v, SIMDE_MM_SHUFFLE(N, N, N, N)));
  return std::bit_cast<i32>(f);
}

/**
 * Returns a lane of an SSE register as f64.
 *
 * @tparam N Lane index, 0 or 1.
 */
template <int N>
f64 m128_f64(const simde__m128d& v) {
  f64 ret;
  simde_mm_store_sd(&ret, simde_mm_shuffle_pd(v, v, SIMDE_MM_SHUFFLE2(N, N)));
  return ret;
}

/**
 * Returns a lane of an SSE register as f64.
 *
 * @tparam N Lane index, 0 or 1.
 * @param v The SSE register, read as two f64 lanes.
 */
template <int N>
f64 m128_f64(const simde__m128& v) {
  return m128_f64<N>(simde_mm_castps_pd(v));
}

/**
 * Returns the bits of a lane of an SSE register as i64.
 *
 * @tparam N Lane index, 0 or 1.
 */
template <int N>
i64 m128_i64(const simde__m128d& v) {
  f64 f;
  simde_mm_store_sd(&f, simde_mm_shuffle_pd(v, v, SIMDE_MM_SHUFFLE2(N, N)));
  return std::bit_cast<i64>(f);
}

/**
 * Returns the bits of a lane of an SSE register as i64.
 *
 * @tparam N Lane index, 0 or 1.
 * @param v The SSE register, read as two 64 bit lanes.
 */
template <int N>
i64 m128_i64(const simde__m128& v) {
  return m128_i64<N>(simde_mm_castps_pd(v));
}

}  // namespace rex::ppc
