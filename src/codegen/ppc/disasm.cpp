/**
 * @file        codegen/disasm.cpp
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

// Binutils PPC_OPCODE_* dialect bits (see thirdparty/disasm/ppc-dis.c)
constexpr uintptr_t kXenonDialect = 0x1          // PPC
                                    | 0x4        // 64
                                    | 0x4000     // POWER4
                                    | 0x8000000  // CELL
                                    | 0x200      // ALTIVEC
                                    | 0x1000000  // VMX_128
                                    | 0x10000;   // CLASSIC

thread_local DisassemblerEngine gBigEndianDisassembler{BFD_ENDIAN_BIG, nullptr};

DisassemblerEngine::DisassemblerEngine(bfd_endian endian, const char* options) {
  INIT_DISASSEMBLE_INFO(info, stdout, fprintf);
  info.arch = bfd_arch_powerpc;
  info.endian = endian;
  info.disassembler_options = options;
  info.private_data = reinterpret_cast<void*>(kXenonDialect);
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
