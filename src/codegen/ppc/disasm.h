/**
 * @file        codegen/ppc/disasm.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <dis-asm.h>
#include <ppc.h>

#include <rex/types.h>

extern "C" {
/**
 * Returns the register class of an operand of an opcode table entry. The class is 1 for a GPR, 2
 * for a GPR where 0 means no register, 3 for an FPR, 4 for a vector register and 0 for any other
 * operand.
 *
 * @param position The operand position, as in the operands array of a decoded instruction.
 */
int OperandRegisterClass(const powerpc_opcode* entry, unsigned position);
}

namespace rex::codegen::detail {

/**
 * Binutils PPC_OPCODE_* dialect bits, the OR of PPC, 64, POWER4, CELL, ALTIVEC, VMX_128 and
 * CLASSIC (see thirdparty/disasm/ppc-dis.c).
 */
constexpr uintptr_t kXenonDialect = 0x1 | 0x4 | 0x4000 | 0x8000000 | 0x200 | 0x1000000 | 0x10000;

/** Decodes raw bytes into a ppc_insn using GNU binutils. */
struct DisassemblerEngine {
  disassemble_info info{};
  DisassemblerEngine(const DisassemblerEngine&) = default;
  DisassemblerEngine& operator=(const DisassemblerEngine&) = delete;

  DisassemblerEngine(bfd_endian endian, const char* options) noexcept;
  ~DisassemblerEngine() = default;

  /**
   * @param out Receives the decoded instruction
   * @return Number of bytes decoded
   */
  int Disassemble(const void* code, size_t size, u64 base, ppc_insn& out);
};

thread_local extern DisassemblerEngine g_big_endian_disassembler;

inline int Disassemble(const void* code, size_t size, u64 base, ppc_insn& out) {
  return g_big_endian_disassembler.Disassemble(code, size, base, out);
}

inline int Disassemble(const void* code, u64 base, ppc_insn& out) {
  return Disassemble(code, 4, base, out);
}

int Disassemble(const void* code, u64 base, ppc_insn* out, size_t nOut);

}  // namespace rex::codegen::detail
