/**
 * @file        tests/unit/memory/bit_cast_copy_test.cpp
 * @brief       Tests that std::bit_cast copies produce the bytes the memcpy copies did
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <bit>
#include <cstring>
#include <string>

#include <rex/bit.h>
#include <rex/string/numeric.h>
#include <rex/types.h>

namespace {

constexpr rex::u32 kF32Patterns[] = {0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u,
                                     0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7FC00001u,
                                     0x00000001u, 0x007FFFFFu, 0xDEADBEEFu};
constexpr rex::u64 kF64Patterns[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x3FF0000000000000ull,
    0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000000ull,
    0x7FF8000000000001ull, 0x0000000000000001ull, 0xDEADBEEFCAFEF00Dull};

template <typename Dst, typename Src>
Dst MemcpyCopy(const Src& src) {
  static_assert(sizeof(Dst) == sizeof(Src));
  Dst dst;
  std::memcpy(&dst, &src, sizeof(Dst));
  return dst;
}

template <typename T>
bool SameBytes(const T& a, const T& b) {
  return std::bit_cast<std::array<rex::u8, sizeof(T)>>(a) ==
         std::bit_cast<std::array<rex::u8, sizeof(T)>>(b);
}

}  // namespace

REX_TEST_CASE("Reinterpret copies the same bytes as memcpy for f32", "[memory][bit_cast]") {
  for (const rex::u32 bits : kF32Patterns) {
    const rex::f32 expected = MemcpyCopy<rex::f32>(bits);
    CHECK(SameBytes(rex::Reinterpret<rex::f32>(bits), expected));
    CHECK(SameBytes(std::bit_cast<rex::f32>(bits), expected));
    rex::f32 out;
    rex::Reinterpret(out, bits);
    CHECK(SameBytes(out, expected));
    CHECK(rex::Reinterpret<rex::u32>(expected) == bits);
  }
}

REX_TEST_CASE("Reinterpret copies the same bytes as memcpy for f64", "[memory][bit_cast]") {
  for (const rex::u64 bits : kF64Patterns) {
    const rex::f64 expected = MemcpyCopy<rex::f64>(bits);
    CHECK(SameBytes(rex::Reinterpret<rex::f64>(bits), expected));
    rex::f64 out;
    rex::Reinterpret(out, bits);
    CHECK(SameBytes(out, expected));
    CHECK(rex::Reinterpret<rex::u64>(expected) == bits);
  }
}

REX_TEST_CASE("to_hex_string of a float prints the bits memcpy would read",
              "[memory][string][bit_cast]") {
  for (const rex::u32 bits : kF32Patterns) {
    CHECK(rex::to_hex_string(MemcpyCopy<rex::f32>(bits)) == rex::to_hex_string(bits));
  }
  for (const rex::u64 bits : kF64Patterns) {
    CHECK(rex::to_hex_string(MemcpyCopy<rex::f64>(bits)) == rex::to_hex_string(bits));
  }
  CHECK(rex::to_hex_string(1.0f) == "3F800000");
  CHECK(rex::to_hex_string(1.0) == "3FF0000000000000");
}

REX_TEST_CASE("from_string hex floats carry the bits memcpy would write",
              "[memory][string][bit_cast]") {
  for (const rex::u32 bits : kF32Patterns) {
    const std::string text = "0x" + rex::to_hex_string(bits);
    CHECK(SameBytes(rex::from_string<rex::f32>(text), MemcpyCopy<rex::f32>(bits)));
  }
  for (const rex::u64 bits : kF64Patterns) {
    const std::string text = "0x" + rex::to_hex_string(bits);
    CHECK(SameBytes(rex::from_string<rex::f64>(text), MemcpyCopy<rex::f64>(bits)));
  }
}
