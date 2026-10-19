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

#include "codegen/ppc/disasm.h"
#include "codegen/ppc/opcode.h"

extern "C" {
extern const powerpc_opcode powerpc_opcodes[];
extern const int powerpc_num_opcodes;
}

namespace {

using rex::u32;
using rex::codegen::detail::Disassemble;
using rex::codegen::detail::IsDropped;
using rex::codegen::detail::LookupID;
using rex::codegen::detail::LookupOpcode;
using rex::codegen::detail::Opcode;

constexpr u32 XForm(u32 primary, u32 xo) {
  return (primary << 26) | (1u << 21) | (3u << 16) | (4u << 11) | (xo << 1);
}

constexpr u32 VMX128Load(u32 xop, u32 vd) {
  return (4u << 26) | ((vd & 31) << 21) | (3u << 16) | (4u << 11) | xop | (((vd >> 5) & 3) << 2);
}

/**
 * Returns the id the disassembler engine gives a word, or 0 when it decodes nothing.
 *
 * @param code The instruction word.
 * @return The engine id.
 */
int EngineID(u32 code) {
  const std::array<rex::u8, 4> bytes = {
      static_cast<rex::u8>(code >> 24), static_cast<rex::u8>(code >> 16),
      static_cast<rex::u8>(code >> 8), static_cast<rex::u8>(code)};
  ppc_insn insn{};
  Disassemble(bytes.data(), 4, 0, insn);
  if (insn.opcode == nullptr) {
    return 0;
  }
  const int id = insn.opcode->id;
  if (id == PPC_INST_VUPKHSB128 && insn.operands[2] == 0x60) {
    return PPC_INST_VUPKHSH128;
  }
  if (id == PPC_INST_VUPKLSB128 && insn.operands[2] == 0x60) {
    return PPC_INST_VUPKLSH128;
  }
  return id;
}

/**
 * Checks that the lookup returns what the engine decodes, with the dropped instructions invalid.
 *
 * @param code The instruction word.
 */
void ExpectSameID(u32 code) {
  const int engine = EngineID(code);
  const int expected = IsDropped(engine) ? 0 : engine;
  const bool same_id = LookupID(code) == expected;
  const bool same_validity = (LookupOpcode(code) == Opcode::kUnknown) == (expected == 0);
  if (!same_id || !same_validity) {
    FAIL_CHECK("word " << std::hex << code << " engine id " << std::dec << expected << " lookup id "
                       << LookupID(code));
  }
}

}  // namespace

REX_TEST_CASE("VMX128 loads decode for every vector register bank", "[codegen][opcode]") {
  for (const u32 vd : {1u, 33u, 65u, 97u}) {
    CAPTURE(vd);
    CHECK(LookupID(VMX128Load(3, vd)) == PPC_INST_LVSL128);
    CHECK(LookupID(VMX128Load(67, vd)) == PPC_INST_LVSR128);
    CHECK(LookupID(VMX128Load(131, vd)) == PPC_INST_LVEWX128);
    CHECK(LookupID(VMX128Load(195, vd)) == PPC_INST_LVX128);
    CHECK(LookupID(VMX128Load(451, vd)) == PPC_INST_STVX128);
    CHECK(LookupID(VMX128Load(707, vd)) == PPC_INST_LVXL128);
  }
}

REX_TEST_CASE("VMX loads and stores decode under primary 31", "[codegen][opcode]") {
  CHECK(LookupID(XForm(31, 6)) == PPC_INST_LVSL);
  CHECK(LookupID(XForm(31, 7)) == PPC_INST_LVEBX);
  CHECK(LookupID(XForm(31, 38)) == PPC_INST_LVSR);
  CHECK(LookupID(XForm(31, 39)) == PPC_INST_LVEHX);
  CHECK(LookupID(XForm(31, 71)) == PPC_INST_LVEWX);
  CHECK(LookupID(XForm(31, 103)) == PPC_INST_LVX);
  CHECK(LookupID(XForm(31, 135)) == PPC_INST_STVEBX);
  CHECK(LookupID(XForm(31, 167)) == PPC_INST_STVEHX);
  CHECK(LookupID(XForm(31, 199)) == PPC_INST_STVEWX);
  CHECK(LookupID(XForm(31, 231)) == PPC_INST_STVX);
  CHECK(LookupID(XForm(31, 359)) == PPC_INST_LVXL);
  CHECK(LookupID(XForm(31, 487)) == PPC_INST_STVXL);
  CHECK(LookupID(XForm(31, 519)) == PPC_INST_LVLX);
  CHECK(LookupID(XForm(31, 551)) == PPC_INST_LVRX);
  CHECK(LookupID(XForm(31, 647)) == PPC_INST_STVLX);
  CHECK(LookupID(XForm(31, 679)) == PPC_INST_STVRX);
}

REX_TEST_CASE("VA form instructions decode at their AltiVec extended opcodes",
              "[codegen][opcode]") {
  constexpr u32 kBase = 0x10611180;
  CHECK(LookupID(kBase | 42) == PPC_INST_VSEL);
  CHECK(LookupID(kBase | 43) == PPC_INST_VPERM);
  CHECK(LookupID(kBase | 44) == PPC_INST_VSLDOI);
  CHECK(LookupID(kBase | 46) == PPC_INST_VMADDFP);
  CHECK(LookupID(kBase | 47) == PPC_INST_VNMSUBFP);
  CHECK(LookupID(kBase | 32) == 0);
  CHECK(LookupID(kBase | 33) == 0);
}

REX_TEST_CASE("AltiVec instructions the Xenon CPU dropped decode as invalid", "[codegen][opcode]") {
  constexpr std::array<u32, 22> kDropped = {
      0x10611008, 0x10611048, 0x10611108, 0x10611148, 0x10611208, 0x10611248,
      0x10611308, 0x10611348, 0x106111A2, 0x106111A4, 0x106111A5, 0x106111A6,
      0x106111A7, 0x106111A8, 0x106111A9, 0x106111A0, 0x106111A1, 0x10611608,
      0x10611708, 0x10611648, 0x10611688, 0x10611788};
  for (const u32 word : kDropped) {
    CAPTURE(word);
    CHECK(LookupID(word) == 0);
    CHECK(LookupOpcode(word) == Opcode::kUnknown);
  }
}

REX_TEST_CASE("VMX forms beside the dropped AltiVec instructions decode", "[codegen][opcode]") {
  CHECK(LookupID(0x10611000) == PPC_INST_VADDUBM);
  CHECK(LookupID(0x10611380) == PPC_INST_VADDSWS);
  CHECK(LookupID(0x106111AA) == PPC_INST_VSEL);
  CHECK(LookupID(0x106111AB) == PPC_INST_VPERM);
  CHECK(LookupID(0x106111AC) == PPC_INST_VSLDOI);
  CHECK(LookupID(0x106111AE) == PPC_INST_VMADDFP);
  CHECK(LookupID(0x106110D3) == PPC_INST_VSLDOI128);
  CHECK(LookupID(VMX128Load(195, 3)) == PPC_INST_LVX128);
  CHECK(LookupID(VMX128Load(451, 3)) == PPC_INST_STVX128);
}

REX_TEST_CASE("The scanner opcodes come from the decoded instruction", "[codegen][opcode]") {
  CHECK(LookupOpcode(0x48000000) == Opcode::b);
  CHECK(LookupOpcode(0x48000001) == Opcode::bl);
  CHECK(LookupOpcode(0x48000002) == Opcode::ba);
  CHECK(LookupOpcode(0x48000003) == Opcode::bla);
  CHECK(LookupOpcode(0x40820010) == Opcode::bc);
  CHECK(LookupOpcode(0x40820011) == Opcode::bcl);
  CHECK(LookupOpcode(0x41820012) == Opcode::bca);
  CHECK(LookupOpcode(0x41820013) == Opcode::bcla);
  CHECK(LookupOpcode(0x4E800020) == Opcode::bclr);
  CHECK(LookupOpcode(0x4E800021) == Opcode::bclrl);
  CHECK(LookupOpcode(0x4D820020) == Opcode::bclr);
  CHECK(LookupOpcode(0x4E800420) == Opcode::bcctr);
  CHECK(LookupOpcode(0x4E800421) == Opcode::bcctrl);
  CHECK(LookupOpcode(0x44000002) == Opcode::sc);
  CHECK(LookupOpcode(0x7FE00008) == Opcode::tw);
  CHECK(LookupOpcode(0x0FE00000) == Opcode::twi);
  CHECK(LookupOpcode(0x38630001) == Opcode::addi);
  CHECK(LookupOpcode(0x38600001) == Opcode::li);
  CHECK(LookupOpcode(0x3C630001) == Opcode::addis);
  CHECK(LookupOpcode(0x3C600001) == Opcode::lis);
  CHECK(LookupOpcode(0x60000000) == Opcode::nop);
  CHECK(LookupOpcode(0x60630001) == Opcode::ori);
  CHECK(LookupOpcode(0x7C632378) == Opcode::or_);
  CHECK(LookupOpcode(0x7C832378) == Opcode::mr);
  CHECK(LookupOpcode(0x7C0802A6) == Opcode::mflr);
  CHECK(LookupOpcode(0x7C0803A6) == Opcode::mtlr);
  CHECK(LookupOpcode(0x7C6902A6) == Opcode::mfctr);
  CHECK(LookupOpcode(0x7C6903A6) == Opcode::mtctr);
  CHECK(LookupOpcode(0x7C6102A6) == Opcode::mfspr);
  CHECK(LookupOpcode(0x7C6103A6) == Opcode::mtspr);
  CHECK(LookupOpcode(0x54630000) == Opcode::rlwinm);
  CHECK(LookupOpcode(0x7C632030) == Opcode::slw);
  CHECK(LookupOpcode(0x7C632430) == Opcode::srw);
  CHECK(LookupOpcode(0x7C632630) == Opcode::sraw);
  CHECK(LookupOpcode(0x7C630E70) == Opcode::srawi);
  CHECK(LookupOpcode(0x7C632050) == Opcode::subf);
  CHECK(LookupOpcode(0x7C632214) == Opcode::add);
  CHECK(LookupOpcode(0x7C632215) == Opcode::add);
  CHECK(LookupOpcode(0x7C632038) == Opcode::and_);
  CHECK(LookupOpcode(0x7C632278) == Opcode::xor_);
  CHECK(LookupOpcode(0x80630000) == Opcode::lwz);
  CHECK(LookupOpcode(0x88630000) == Opcode::lbz);
  CHECK(LookupOpcode(0xA0630000) == Opcode::lhz);
  CHECK(LookupOpcode(0x94210000) == Opcode::stwu);
  CHECK(LookupOpcode(0x7C63202E) == Opcode::lwzx);
  CHECK(LookupOpcode(0x7C6320AE) == Opcode::lbzx);
  CHECK(LookupOpcode(0x7C63222E) == Opcode::lhzx);
  CHECK(LookupOpcode(0x2C030000) == Opcode::cmpi);
  CHECK(LookupOpcode(0x28030000) == Opcode::cmpli);
  CHECK(LookupOpcode(0x10611380) == Opcode::kOther);
  CHECK(LookupOpcode(0x00000000) == Opcode::kUnknown);
}

REX_TEST_CASE("The SH form of vupkhsb128 and vupklsb128 resolves to the halfword ids",
              "[codegen][opcode]") {
  bool saw_high = false;
  bool saw_low = false;
  for (u32 low = 0; low < 2048 && !(saw_high && saw_low); ++low) {
    for (const u32 fill : {0x00000000u, 0x00600000u, 0x03E0F800u}) {
      const u32 word = (6u << 26) | fill | low;
      const int id = LookupID(word);
      saw_high |= id == PPC_INST_VUPKHSH128;
      saw_low |= id == PPC_INST_VUPKLSH128;
    }
  }
  CHECK(saw_high);
  CHECK(saw_low);
}

REX_TEST_CASE("The lookup matches the disassembler on every entry of the opcode table",
              "[codegen][opcode]") {
  const u32 regs = (3u << 21) | (1u << 16) | (2u << 11) | (6u << 6);
  for (int i = 0; i < powerpc_num_opcodes; ++i) {
    const powerpc_opcode& entry = powerpc_opcodes[i];
    const u32 base = static_cast<u32>(entry.opcode);
    const u32 mask = static_cast<u32>(entry.mask);
    ExpectSameID(base);
    ExpectSameID(base | (regs & ~mask));
  }
}

REX_TEST_CASE("The lookup matches the disassembler on the vector opcode space",
              "[codegen][opcode]") {
  const u32 regs = (3u << 21) | (1u << 16) | (2u << 11) | (6u << 6);
  for (const u32 primary : {4u, 5u, 6u}) {
    for (const u32 fill : {0u, regs & ~0x7FFu, 0x03FFF800u, 0x0155F800u}) {
      for (u32 low = 0; low < 2048; ++low) {
        ExpectSameID((primary << 26) | (fill & 0x03FFF800u) | low);
      }
    }
  }
}

REX_TEST_CASE("The lookup matches the disassembler on a fixed sample of words",
              "[codegen][opcode]") {
  u32 state = 0x5EEDC0DEu;
  for (u32 i = 0; i < 1000000; ++i) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    ExpectSameID(state);
  }
}
