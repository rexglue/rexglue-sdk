/**
 * @file        tests/unit/codegen/opcode_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>

#include "codegen/ppc/opcode.h"

namespace {

using rex::u32;
using rex::codegen::detail::LookupOpcode;
using rex::codegen::detail::Opcode;

constexpr u32 XForm(u32 primary, u32 xo) {
  return (primary << 26) | (1u << 21) | (3u << 16) | (4u << 11) | (xo << 1);
}

constexpr u32 VMX128Load(u32 xop, u32 vd) {
  return (4u << 26) | ((vd & 31) << 21) | (3u << 16) | (4u << 11) | xop | (((vd >> 5) & 3) << 2);
}

}  // namespace

REX_TEST_CASE("VMX128 loads decode for every vector register bank", "[codegen][opcode]") {
  for (const u32 vd : {1u, 33u, 65u, 97u}) {
    CAPTURE(vd);
    CHECK(LookupOpcode(VMX128Load(3, vd)) == Opcode::lvsl128);
    CHECK(LookupOpcode(VMX128Load(67, vd)) == Opcode::lvsr128);
    CHECK(LookupOpcode(VMX128Load(131, vd)) == Opcode::lvewx128);
    CHECK(LookupOpcode(VMX128Load(195, vd)) == Opcode::lvx128);
    CHECK(LookupOpcode(VMX128Load(707, vd)) == Opcode::lvxl128);
  }
}

REX_TEST_CASE("VMX loads and stores decode under primary 31", "[codegen][opcode]") {
  CHECK(LookupOpcode(XForm(31, 6)) == Opcode::lvsl);
  CHECK(LookupOpcode(XForm(31, 7)) == Opcode::lvebx);
  CHECK(LookupOpcode(XForm(31, 38)) == Opcode::lvsr);
  CHECK(LookupOpcode(XForm(31, 39)) == Opcode::lvehx);
  CHECK(LookupOpcode(XForm(31, 71)) == Opcode::lvewx);
  CHECK(LookupOpcode(XForm(31, 103)) == Opcode::lvx);
  CHECK(LookupOpcode(XForm(31, 135)) == Opcode::stvebx);
  CHECK(LookupOpcode(XForm(31, 167)) == Opcode::stvehx);
  CHECK(LookupOpcode(XForm(31, 199)) == Opcode::stvewx);
  CHECK(LookupOpcode(XForm(31, 231)) == Opcode::stvx);
  CHECK(LookupOpcode(XForm(31, 359)) == Opcode::lvxl);
  CHECK(LookupOpcode(XForm(31, 487)) == Opcode::stvxl);
  CHECK(LookupOpcode(XForm(31, 519)) == Opcode::lvlx);
  CHECK(LookupOpcode(XForm(31, 551)) == Opcode::lvrx);
  CHECK(LookupOpcode(XForm(31, 647)) == Opcode::stvlx);
  CHECK(LookupOpcode(XForm(31, 679)) == Opcode::stvrx);
}

REX_TEST_CASE("VA form instructions decode at their AltiVec extended opcodes",
              "[codegen][opcode]") {
  constexpr u32 kBase = 0x10611180;
  CHECK(LookupOpcode(kBase | 42) == Opcode::vsel);
  CHECK(LookupOpcode(kBase | 43) == Opcode::vperm);
  CHECK(LookupOpcode(kBase | 44) == Opcode::vsldoi);
  CHECK(LookupOpcode(kBase | 46) == Opcode::vmaddfp);
  CHECK(LookupOpcode(kBase | 47) == Opcode::vnmsubfp);
  CHECK(LookupOpcode(kBase | 32) != Opcode::vmaddfp);
  CHECK(LookupOpcode(kBase | 33) != Opcode::vnmsubfp);
}

REX_TEST_CASE("AltiVec instructions the Xenon CPU dropped decode as unknown", "[codegen][opcode]") {
  constexpr std::array<u32, 22> kDropped = {
      0x10611008, 0x10611048, 0x10611108, 0x10611148, 0x10611208, 0x10611248,
      0x10611308, 0x10611348, 0x106111A2, 0x106111A4, 0x106111A5, 0x106111A6,
      0x106111A7, 0x106111A8, 0x106111A9, 0x106111A0, 0x106111A1, 0x10611608,
      0x10611708, 0x10611648, 0x10611688, 0x10611788};
  for (const u32 word : kDropped) {
    CAPTURE(word);
    CHECK(LookupOpcode(word) == Opcode::kUnknown);
  }
}

REX_TEST_CASE("VMX forms beside the dropped AltiVec instructions still decode",
              "[codegen][opcode]") {
  CHECK(LookupOpcode(0x10611000) == Opcode::vaddubm);
  CHECK(LookupOpcode(0x106111AA) == Opcode::vsel);
  CHECK(LookupOpcode(0x106111AB) == Opcode::vperm);
  CHECK(LookupOpcode(0x106111AC) == Opcode::vsldoi);
  CHECK(LookupOpcode(0x106111AE) == Opcode::vmaddfp);
  CHECK(LookupOpcode(0x106110D3) == Opcode::vsldoi128);
  CHECK(LookupOpcode(VMX128Load(195, 3)) == Opcode::lvx128);
  CHECK(LookupOpcode(VMX128Load(451, 3)) == Opcode::stvx128);
}
