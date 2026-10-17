/**
 * @file        tests/unit/core/math_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <limits>

#include <rex/math.h>
#include <rex/types.h>

REX_TEST_CASE("lzcnt counts leading zero bits at every width", "[core][math]") {
  CHECK(rex::lzcnt(u8{0}) == 8);
  CHECK(rex::lzcnt(u8{1}) == 7);
  CHECK(rex::lzcnt(u8{0x10}) == 3);
  CHECK(rex::lzcnt(u8{0x80}) == 0);
  CHECK(rex::lzcnt(u16{0}) == 16);
  CHECK(rex::lzcnt(u16{1}) == 15);
  CHECK(rex::lzcnt(u16{0x00F0}) == 8);
  CHECK(rex::lzcnt(u16{0x8000}) == 0);
  CHECK(rex::lzcnt(u32{0}) == 32);
  CHECK(rex::lzcnt(u32{1}) == 31);
  CHECK(rex::lzcnt(u32{0x00010000}) == 15);
  CHECK(rex::lzcnt(u32{0x80000000}) == 0);
  CHECK(rex::lzcnt(u64{0}) == 64);
  CHECK(rex::lzcnt(u64{1}) == 63);
  CHECK(rex::lzcnt(u64{0x0000000100000000}) == 31);
  CHECK(rex::lzcnt(u64{0x8000000000000000}) == 0);
}

REX_TEST_CASE("tzcnt counts trailing zero bits at every width", "[core][math]") {
  CHECK(rex::tzcnt(u8{0}) == 8);
  CHECK(rex::tzcnt(u8{1}) == 0);
  CHECK(rex::tzcnt(u8{0x18}) == 3);
  CHECK(rex::tzcnt(u8{0x80}) == 7);
  CHECK(rex::tzcnt(u16{0}) == 16);
  CHECK(rex::tzcnt(u16{0x0100}) == 8);
  CHECK(rex::tzcnt(u16{0x8000}) == 15);
  CHECK(rex::tzcnt(u32{0}) == 32);
  CHECK(rex::tzcnt(u32{0x00010000}) == 16);
  CHECK(rex::tzcnt(u32{0x80000000}) == 31);
  CHECK(rex::tzcnt(u64{0}) == 64);
  CHECK(rex::tzcnt(u64{0x0000000100000000}) == 32);
  CHECK(rex::tzcnt(u64{0x8000000000000000}) == 63);
}

REX_TEST_CASE("signed overloads read the value as unsigned", "[core][math]") {
  CHECK(rex::lzcnt(i8{-1}) == 0);
  CHECK(rex::lzcnt(i16{1}) == 15);
  CHECK(rex::lzcnt(i32{1}) == 31);
  CHECK(rex::lzcnt(i64{-1}) == 0);
  CHECK(rex::tzcnt(i8{0}) == 8);
  CHECK(rex::tzcnt(i16{-32768}) == 15);
  CHECK(rex::tzcnt(i32{-2}) == 1);
  CHECK(rex::tzcnt(i64{0}) == 64);
}

REX_TEST_CASE("bit_scan_forward finds the lowest set bit", "[core][math]") {
  u32 index = 0;
  CHECK_FALSE(rex::bit_scan_forward(u32{0}, &index));
  CHECK(rex::bit_scan_forward(u32{0x10}, &index));
  CHECK(index == 4);
  CHECK(rex::bit_scan_forward(u32{0x80000000}, &index));
  CHECK(index == 31);
  CHECK_FALSE(rex::bit_scan_forward(u64{0}, &index));
  CHECK(rex::bit_scan_forward(u64{1} << 40, &index));
  CHECK(index == 40);
  CHECK(rex::bit_scan_forward(i32{-1}, &index));
  CHECK(index == 0);
  CHECK(rex::bit_scan_forward(i64{-8}, &index));
  CHECK(index == 3);
}

REX_TEST_CASE("mul_div keeps the exact quotient of a product past u64", "[core][math]") {
  CHECK(rex::mul_div(u64{1} << 62, 12, 8) == 0x6000000000000000);
  CHECK(rex::mul_div(std::numeric_limits<u64>::max(), 3, 4) == 0xBFFFFFFFFFFFFFFF);
}

REX_TEST_CASE("mul_div saturates a quotient past u64", "[core][math]") {
  CHECK(rex::mul_div(u64{1} << 63, 4, 2) == std::numeric_limits<u64>::max());
}

REX_TEST_CASE("mul_div divides by a denominator with its top bit set", "[core][math]") {
  CHECK(rex::mul_div(std::numeric_limits<u64>::max(), 0x8000000000000001, 0x8000000000000003) ==
        0xFFFFFFFFFFFFFFFB);
}

REX_TEST_CASE("mul_div keeps the largest quotient that fits", "[core][math]") {
  constexpr u64 kDenominator = 0x123456789;
  CHECK(rex::mul_div(u64{1} << 63, 2 * (kDenominator - 1), kDenominator) == 0xFFFFFFFF1EFFFFFF);
}

REX_TEST_CASE("mul_div saturates when the high half reaches the denominator", "[core][math]") {
  constexpr u64 kDenominator = 0x123456789;
  CHECK(rex::mul_div(u64{1} << 63, 2 * kDenominator, kDenominator) ==
        std::numeric_limits<u64>::max());
}

REX_TEST_CASE("mul_div by a denominator of 1 is the product", "[core][math]") {
  CHECK(rex::mul_div(0xFFFFFFFF, 0xFFFFFFFF, 1) == 0xFFFFFFFE00000001);
}

static_assert(rex::mul_div(u64{1} << 62, 12, 8) == 0x6000000000000000);

REX_TEST_CASE("mul_div matches plain arithmetic for small ratios", "[core][math]") {
  CHECK(rex::mul_div(1'000'003, 5, 1) == 5'000'015);
  CHECK(rex::mul_div(1'000'003, 1, 20) == 50'000);
}

REX_TEST_CASE("round_up leaves 0 at 0 as Align does", "[core][math]") {
  CHECK(rex::round_up(0u, 16u) == 0u);
  CHECK(rex::Align(0u, 16u) == 0u);
  CHECK(rex::round_up(1u, 16u) == 16u);
  CHECK(rex::round_up(33u, 16u) == 48u);
  CHECK(rex::round_up(48u, 16u) == 48u);
  CHECK(rex::round_up(10u, 3u) == 12u);
}

REX_TEST_CASE("round_up_nonzero turns 0 into 1 multiple", "[core][math]") {
  CHECK(rex::round_up_nonzero(0u, 16u) == 16u);
  CHECK(rex::round_up_nonzero(1u, 16u) == 16u);
  CHECK(rex::round_up_nonzero(48u, 16u) == 48u);
  CHECK(rex::round_up_nonzero(10u, 3u) == 12u);
}
