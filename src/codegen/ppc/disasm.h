/**
 * @file        rexcodegen/internal/ppc/disasm.h
 * @brief       PPC disassembler interface
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

namespace rex::codegen::ppc {

//=============================================================================
// Low-level Disassembler Engine (binutils wrapper)
//=============================================================================
// Decodes raw bytes into ppc_insn struct using GNU binutils

struct DisassemblerEngine {
  disassemble_info info{};
  DisassemblerEngine(const DisassemblerEngine&) = default;
  DisassemblerEngine& operator=(const DisassemblerEngine&) = delete;

  DisassemblerEngine(bfd_endian endian, const char* options);
  ~DisassemblerEngine() = default;

  /**
   * Disassemble a single instruction
   * @return Number of bytes decoded
   */
  int Disassemble(const void* code, size_t size, uint64_t base, ppc_insn& out);
};

thread_local extern DisassemblerEngine gBigEndianDisassembler;

inline int Disassemble(const void* code, size_t size, uint64_t base, ppc_insn& out) {
  return gBigEndianDisassembler.Disassemble(code, size, base, out);
}

inline int Disassemble(const void* code, uint64_t base, ppc_insn& out) {
  return Disassemble(code, 4, base, out);
}

int Disassemble(const void* code, uint64_t base, ppc_insn* out, size_t nOut);

}  // namespace rex::codegen::ppc
