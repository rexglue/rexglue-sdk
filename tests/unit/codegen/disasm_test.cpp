/**
 * @file        tests/unit/codegen/disasm_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <array>

#include "codegen/ppc/disasm.h"

TEST_CASE("Disassemble decodes nothing from a size above the int range", "[codegen][disasm]") {
  const std::array<rex::u8, 4> nop{0x60, 0x00, 0x00, 0x00};
  ppc_insn insn{};
  CHECK(rex::codegen::ppc::Disassemble(nop.data(), 4, 0x82000000, insn) == 4);
  CHECK(rex::codegen::ppc::Disassemble(nop.data(), 0x80000000, 0x82000000, insn) == 0);
}
