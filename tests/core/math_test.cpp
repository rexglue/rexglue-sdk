/**
 * @file        tests/core/math_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/math.h>
#include <rex/types.h>

TEST_CASE("lzcnt counts leading zero bits at every width", "[core][math]") {
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

TEST_CASE("tzcnt counts trailing zero bits at every width", "[core][math]") {
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

TEST_CASE("signed overloads read the value as unsigned", "[core][math]") {
  CHECK(rex::lzcnt(i8{-1}) == 0);
  CHECK(rex::lzcnt(i16{1}) == 15);
  CHECK(rex::lzcnt(i32{1}) == 31);
  CHECK(rex::lzcnt(i64{-1}) == 0);
  CHECK(rex::tzcnt(i8{0}) == 8);
  CHECK(rex::tzcnt(i16{-32768}) == 15);
  CHECK(rex::tzcnt(i32{-2}) == 1);
  CHECK(rex::tzcnt(i64{0}) == 64);
}

TEST_CASE("bit_scan_forward finds the lowest set bit", "[core][math]") {
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
