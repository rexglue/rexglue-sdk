/**
 * @file        tests/unit/ppc/saturation_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <cstddef>
#include <cstring>
#include <set>
#include <vector>

#include "rex_test.h"

#include <rex/ppc/vmx.h>

#include "pack_d3d_cases.h"

namespace {

using BinaryFunction = bool (*)(simde__m128i, simde__m128i);

#if defined(__SANITIZE_THREAD__)
inline constexpr bool kThreadSanitized = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
inline constexpr bool kThreadSanitized = true;
#else
inline constexpr bool kThreadSanitized = false;
#endif
#else
inline constexpr bool kThreadSanitized = false;
#endif

/** Returns every value below count, or a strided sample plus the edges under ThreadSanitizer. */
std::vector<u32> SweptValues(u32 count, u32 stride, const std::vector<u32>& edges) {
  std::set<u32> picked;
  if (!kThreadSanitized) {
    for (u32 i = 0; i < count; ++i) {
      picked.insert(i);
    }
  } else {
    for (u32 i = 0; i < count; i += stride) {
      picked.insert(i);
    }
    for (const u32 edge : edges) {
      for (const i64 delta : {-1, 0, 1}) {
        const i64 value = static_cast<i64>(edge) + delta;
        if (value >= 0 && value < count) {
          picked.insert(static_cast<u32>(value));
        }
      }
    }
  }
  return {picked.begin(), picked.end()};
}

std::vector<u32> OperandValues(u32 bits) {
  std::vector<u32> values;
  if (bits == 8) {
    return SweptValues(256, 37, {0x00, 0x7F, 0x80, 0xFF});
  }
  const u32 top = bits == 16 ? 0xFFFFu : 0xFFFFFFFFu;
  const u32 half = (top >> 1) + 1;
  for (const u32 base : {0u, half, top}) {
    for (u32 delta = 0; delta < 4; ++delta) {
      values.push_back((base + delta) & top);
      values.push_back((base - delta) & top);
    }
  }
  values.push_back(top / 3);
  values.push_back(top / 3 * 2);
  return values;
}

i64 Widen(u32 pattern, u32 bits, bool is_signed) {
  if (is_signed && (pattern >> (bits - 1)) != 0) {
    return static_cast<i64>(pattern) - (i64{1} << bits);
  }
  return static_cast<i64>(pattern);
}

struct Mismatch {
  bool found = false;
  u32 x = 0;
  u32 y = 0;
  size_t position = 0;
  size_t operand = 0;
};

void CheckPairs(BinaryFunction check, u32 bits, bool is_signed, bool is_subtract) {
  const std::vector<u32> values = OperandValues(bits);
  const size_t elements = 128 / bits;
  const i64 low = is_signed ? -(i64{1} << (bits - 1)) : 0;
  const i64 high = is_signed ? (i64{1} << (bits - 1)) - 1 : (i64{1} << bits) - 1;
  Mismatch mismatch;
  for (const u32 x : values) {
    for (const u32 y : values) {
      const i64 a = Widen(x, bits, is_signed);
      const i64 b = Widen(y, bits, is_signed);
      const i64 exact = is_subtract ? a - b : a + b;
      const bool expected = exact < low || exact > high;
      for (size_t position = 0; position < elements; ++position) {
        alignas(16) std::array<u8, 16> left{};
        alignas(16) std::array<u8, 16> right{};
        std::memcpy(left.data() + position * (bits / 8), &x, bits / 8);
        std::memcpy(right.data() + position * (bits / 8), &y, bits / 8);
        const bool got =
            check(simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(left.data())),
                  simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(right.data())));
        if (got != expected && !mismatch.found) {
          mismatch = {true, x, y, position, 0};
        }
      }
    }
  }
  CAPTURE(bits, is_signed, mismatch.x, mismatch.y, mismatch.position);
  REQUIRE_FALSE(mismatch.found);
}

void CheckPack(BinaryFunction check, u32 bits, bool is_signed_source, bool is_signed_dest) {
  std::vector<u32> values = OperandValues(bits);
  if (bits == 16) {
    values =
        SweptValues(65536, 251, {0x0000, 0x007F, 0x0080, 0x00FF, 0x0100, 0x7FFF, 0x8000, 0xFFFF});
  } else {
    for (const u32 v :
         {0x7Fu, 0x80u, 0xFFu, 0x100u, 0x7FFFu, 0x8000u, 0xFFFFu, 0x10000u, 0xFFFF8000u,
          0xFFFF7FFFu, 0xFFFF0000u, 0xFFFFFF80u, 0xFFFFFF7Fu, 0xFFFFFF00u}) {
      values.push_back(v);
    }
  }
  const u32 dest_bits = bits / 2;
  const i64 low = is_signed_dest ? -(i64{1} << (dest_bits - 1)) : 0;
  const i64 high = (i64{1} << dest_bits) - 1 - (is_signed_dest ? (i64{1} << (dest_bits - 1)) : 0);
  const size_t elements = 128 / bits;
  Mismatch mismatch;
  for (const u32 x : values) {
    const i64 value = Widen(x, bits, is_signed_source);
    const bool expected = value < low || value > high;
    for (size_t operand = 0; operand < 2; ++operand) {
      for (size_t position = 0; position < elements; ++position) {
        alignas(16) std::array<u8, 16> bytes[2] = {};
        std::memcpy(bytes[operand].data() + position * (bits / 8), &x, bits / 8);
        const bool got =
            check(simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(bytes[0].data())),
                  simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(bytes[1].data())));
        if (got != expected && !mismatch.found) {
          mismatch = {true, x, 0, position, operand};
        }
      }
    }
  }
  CAPTURE(bits, is_signed_source, is_signed_dest, mismatch.x, mismatch.operand, mismatch.position);
  REQUIRE_FALSE(mismatch.found);
}

}  // namespace

REX_TEST_CASE("IsAddSaturating reports the clamped elements of the byte adds",
              "[ppc][saturation]") {
  CheckPairs(rex::ppc::simd::IsAddSaturating<8, true>, 8, true, false);
  CheckPairs(rex::ppc::simd::IsAddSaturating<8, false>, 8, false, false);
}

REX_TEST_CASE("IsAddSaturating reports the clamped elements of the halfword adds",
              "[ppc][saturation]") {
  CheckPairs(rex::ppc::simd::IsAddSaturating<16, true>, 16, true, false);
  CheckPairs(rex::ppc::simd::IsAddSaturating<16, false>, 16, false, false);
}

REX_TEST_CASE("IsAddSaturating reports the clamped elements of the word adds",
              "[ppc][saturation]") {
  CheckPairs(rex::ppc::simd::IsAddSaturating<32, true>, 32, true, false);
  CheckPairs(rex::ppc::simd::IsAddSaturating<32, false>, 32, false, false);
}

REX_TEST_CASE("IsSubSaturating reports the clamped elements of the byte subtracts",
              "[ppc][saturation]") {
  CheckPairs(rex::ppc::simd::IsSubSaturating<8, true>, 8, true, true);
  CheckPairs(rex::ppc::simd::IsSubSaturating<8, false>, 8, false, true);
}

REX_TEST_CASE("IsSubSaturating reports the clamped elements of the halfword subtracts",
              "[ppc][saturation]") {
  CheckPairs(rex::ppc::simd::IsSubSaturating<16, true>, 16, true, true);
  CheckPairs(rex::ppc::simd::IsSubSaturating<16, false>, 16, false, true);
}

REX_TEST_CASE("IsSubSaturating reports the clamped elements of the word subtracts",
              "[ppc][saturation]") {
  CheckPairs(rex::ppc::simd::IsSubSaturating<32, true>, 32, true, true);
  CheckPairs(rex::ppc::simd::IsSubSaturating<32, false>, 32, false, true);
}

REX_TEST_CASE("IsPackSaturating reports the clamped elements of the halfword packs",
              "[ppc][saturation]") {
  CheckPack(rex::ppc::simd::IsPackSaturating<16, false>, 16, false, false);
  CheckPack(rex::ppc::simd::IsPackSaturating<16, false>, 16, true, false);
  CheckPack(rex::ppc::simd::IsPackSaturating<16, true>, 16, true, true);
}

REX_TEST_CASE("IsPackSaturating reports the clamped elements of the word packs",
              "[ppc][saturation]") {
  CheckPack(rex::ppc::simd::IsPackSaturating<32, false>, 32, false, false);
  CheckPack(rex::ppc::simd::IsPackSaturating<32, false>, 32, true, false);
  CheckPack(rex::ppc::simd::IsPackSaturating<32, true>, 32, true, true);
}

REX_TEST_CASE("IsPackD3DSaturating matches the captured saturation of every pack format",
              "[ppc][saturation]") {
  for (const PackD3DCase& expected : kPackD3DCases) {
    alignas(16) std::array<u32, 4> words;
    for (size_t i = 0; i < 4; ++i) {
      words[3 - i] = expected.source[i];
    }
    const simde__m128 source = simde_mm_castsi128_ps(
        simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(words.data())));
    bool got = false;
    switch (expected.type) {
      case 0:
        got = rex::ppc::simd::IsPackD3DSaturating<0>(source);
        break;
      case 1:
        got = rex::ppc::simd::IsPackD3DSaturating<1>(source);
        break;
      case 2:
        got = rex::ppc::simd::IsPackD3DSaturating<2>(source);
        break;
      case 3:
        got = rex::ppc::simd::IsPackD3DSaturating<3>(source);
        break;
      case 4:
        got = rex::ppc::simd::IsPackD3DSaturating<4>(source);
        break;
      case 5:
        got = rex::ppc::simd::IsPackD3DSaturating<5>(source);
        break;
      default:
        got = rex::ppc::simd::IsPackD3DSaturating<6>(source);
        break;
    }
    CAPTURE(expected.type, expected.source[0], expected.source[1], expected.source[2],
            expected.source[3]);
    CHECK(got == expected.saturated);
  }
}
