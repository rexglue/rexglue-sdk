/**
 * @file        tests/unit/ppc/vector_unaligned_write_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <cstring>
#include <vector>

#include "rex_test.h"

#include <rex/ppc/vmx.h>

namespace {

using Lanes = std::array<u8, 16>;

Lanes MakeLanes() {
  Lanes lanes{};
  for (u32 i = 0; i < 16; ++i) {
    lanes[i] = static_cast<u8>(0xA0 + i);
  }
  return lanes;
}

simde__m128i Load(const Lanes& lanes) {
  return simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(lanes.data()));
}

std::vector<u8> MakeBuffer(size_t size) {
  std::vector<u8> buffer(size);
  for (size_t i = 0; i < size; ++i) {
    buffer[i] = static_cast<u8>(i);
  }
  return buffer;
}

void ReferenceLeft(u8* buffer, u32 ea, const Lanes& lanes) {
  for (u32 i = 0; i < 16 - (ea & 0xF); ++i) {
    buffer[ea + i] = lanes[15 - i];
  }
}

void ReferenceRight(u8* buffer, u32 ea, const Lanes& lanes) {
  for (u32 i = 0; i < (ea & 0xF); ++i) {
    buffer[ea - i - 1] = lanes[i];
  }
}

}  // namespace

REX_TEST_CASE("StoreVectorLeft writes the bytes of the stvlx loop for every alignment",
              "[ppc][simd]") {
  const Lanes lanes = MakeLanes();
  for (u32 ea = 16; ea < 32; ++ea) {
    auto expected = MakeBuffer(64);
    ReferenceLeft(expected.data(), ea, lanes);
    auto actual = MakeBuffer(64);
    rex::ppc::simd::StoreVectorLeft(actual.data() + ea, ea, Load(lanes));
    CAPTURE(ea);
    CHECK(actual == expected);
  }
}

REX_TEST_CASE("StoreVectorRight writes the bytes of the stvrx loop for every alignment",
              "[ppc][simd]") {
  const Lanes lanes = MakeLanes();
  for (u32 ea = 16; ea < 32; ++ea) {
    auto expected = MakeBuffer(64);
    ReferenceRight(expected.data(), ea, lanes);
    auto actual = MakeBuffer(64);
    rex::ppc::simd::StoreVectorRight(actual.data() + (ea & ~0xFu), ea, Load(lanes));
    CAPTURE(ea);
    CHECK(actual == expected);
  }
}

REX_TEST_CASE("StoreVectorRight at an aligned address writes nothing", "[ppc][simd]") {
  const Lanes lanes = MakeLanes();
  auto actual = MakeBuffer(64);
  rex::ppc::simd::StoreVectorRight(actual.data() + 32, 32, Load(lanes));
  CHECK(actual == MakeBuffer(64));
}

REX_TEST_CASE("StoreVectorPair stores one contiguous span when the quadwords meet", "[ppc][simd]") {
  const Lanes lanes = MakeLanes();
  for (u32 ea = 16; ea < 32; ++ea) {
    auto expected = MakeBuffer(64);
    ReferenceLeft(expected.data(), ea, lanes);
    ReferenceRight(expected.data(), ea + 16, lanes);
    auto actual = MakeBuffer(64);
    rex::ppc::simd::StoreVectorPair(actual.data() + ea, ea, actual.data() + ((ea + 16) & ~0xFu),
                                    ea + 16, Load(lanes));
    CAPTURE(ea);
    CHECK(actual == expected);
  }
}

REX_TEST_CASE("StoreVectorPair falls back when the right address is not the next quadword",
              "[ppc][simd]") {
  const Lanes lanes = MakeLanes();
  constexpr u32 kLeft = 21;
  constexpr u32 kRight = kLeft + 20;
  auto expected = MakeBuffer(96);
  ReferenceLeft(expected.data(), kLeft, lanes);
  ReferenceRight(expected.data(), kRight, lanes);
  auto actual = MakeBuffer(96);
  rex::ppc::simd::StoreVectorPair(actual.data() + kLeft, kLeft, actual.data() + (kRight & ~0xFu),
                                  kRight, Load(lanes));
  CHECK(actual == expected);
}

REX_TEST_CASE("StoreVectorPair falls back when the host span is not contiguous", "[ppc][simd]") {
  const Lanes lanes = MakeLanes();
  constexpr u32 kLeft = 21;
  constexpr u32 kRight = 37;
  constexpr size_t kJump = 0x1000;
  auto expected = MakeBuffer(0x1100);
  ReferenceLeft(expected.data(), kLeft, lanes);
  ReferenceRight(expected.data() + kJump, kRight, lanes);
  auto actual = MakeBuffer(0x1100);
  rex::ppc::simd::StoreVectorPair(actual.data() + kLeft, kLeft,
                                  actual.data() + kJump + (kRight & ~0xFu), kRight, Load(lanes));
  CHECK(actual == expected);
}
