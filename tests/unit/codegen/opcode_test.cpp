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

#include "codegen/ppc/opcode.h"

namespace {

using rex::u32;
using rex::codegen::detail::lookup_opcode;
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
    CHECK(lookup_opcode(VMX128Load(3, vd)) == Opcode::lvsl128);
    CHECK(lookup_opcode(VMX128Load(67, vd)) == Opcode::lvsr128);
    CHECK(lookup_opcode(VMX128Load(131, vd)) == Opcode::lvewx128);
    CHECK(lookup_opcode(VMX128Load(195, vd)) == Opcode::lvx128);
    CHECK(lookup_opcode(VMX128Load(707, vd)) == Opcode::lvxl128);
  }
}

REX_TEST_CASE("VMX loads and stores decode under primary 31", "[codegen][opcode]") {
  CHECK(lookup_opcode(XForm(31, 6)) == Opcode::lvsl);
  CHECK(lookup_opcode(XForm(31, 7)) == Opcode::lvebx);
  CHECK(lookup_opcode(XForm(31, 38)) == Opcode::lvsr);
  CHECK(lookup_opcode(XForm(31, 39)) == Opcode::lvehx);
  CHECK(lookup_opcode(XForm(31, 71)) == Opcode::lvewx);
  CHECK(lookup_opcode(XForm(31, 103)) == Opcode::lvx);
  CHECK(lookup_opcode(XForm(31, 135)) == Opcode::stvebx);
  CHECK(lookup_opcode(XForm(31, 167)) == Opcode::stvehx);
  CHECK(lookup_opcode(XForm(31, 199)) == Opcode::stvewx);
  CHECK(lookup_opcode(XForm(31, 231)) == Opcode::stvx);
  CHECK(lookup_opcode(XForm(31, 359)) == Opcode::lvxl);
  CHECK(lookup_opcode(XForm(31, 487)) == Opcode::stvxl);
  CHECK(lookup_opcode(XForm(31, 519)) == Opcode::lvlx);
  CHECK(lookup_opcode(XForm(31, 551)) == Opcode::lvrx);
  CHECK(lookup_opcode(XForm(31, 647)) == Opcode::stvlx);
  CHECK(lookup_opcode(XForm(31, 679)) == Opcode::stvrx);
}
