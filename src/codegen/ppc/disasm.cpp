/**
 * @file        rexcodegen/disasm.cpp
 * @brief       PPC disassembly implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "disasm.h"

namespace rex::codegen::ppc {

thread_local DisassemblerEngine gBigEndianDisassembler{BFD_ENDIAN_BIG, "cell 64"};

DisassemblerEngine::DisassemblerEngine(bfd_endian endian, const char* options) {
  INIT_DISASSEMBLE_INFO(info, stdout, fprintf);
  info.arch = bfd_arch_powerpc;
  info.endian = endian;
  info.disassembler_options = options;
}

int DisassemblerEngine::Disassemble(const void* code, size_t size, uint64_t base, ppc_insn& out) {
  if (size < 4) {
    return 0;
  }

  info.buffer = (bfd_byte*)code;
  info.buffer_vma = base;
  info.buffer_length = size;
  return decode_insn_ppc(base, &info, &out);
}

int Disassemble(const void* code, uint64_t base, ppc_insn* out, size_t nOut) {
  for (size_t i = 0; i < nOut; i++) {
    Disassemble(static_cast<const uint32_t*>(code) + i, base, out[i]);
  }
  return static_cast<int>(nOut) * 4;
}

}  // namespace rex::codegen::ppc
