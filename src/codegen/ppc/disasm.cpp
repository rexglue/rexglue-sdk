#include "disasm.h"

#include <limits>

#include <rex/types.h>

namespace rex::codegen::ppc {

/**
 * Binutils PPC_OPCODE_* dialect bits, the OR of PPC, 64, POWER4, CELL, ALTIVEC, VMX_128 and
 * CLASSIC (see thirdparty/disasm/ppc-dis.c).
 */
constexpr uintptr_t kXenonDialect = 0x1 | 0x4 | 0x4000 | 0x8000000 | 0x200 | 0x1000000 | 0x10000;

thread_local DisassemblerEngine gBigEndianDisassembler{BFD_ENDIAN_BIG, nullptr};

DisassemblerEngine::DisassemblerEngine(bfd_endian endian, const char* options) noexcept {
  INIT_DISASSEMBLE_INFO(info, stdout, fprintf);
  info.arch = bfd_arch_powerpc;
  info.endian = endian;
  info.disassembler_options = options;
  // NOLINTNEXTLINE(performance-no-int-to-ptr): disassemble_info carries the dialect as a pointer
  info.private_data = reinterpret_cast<void*>(kXenonDialect);
}

int DisassemblerEngine::Disassemble(const void* code, size_t size, u64 base, ppc_insn& out) {
  if (size < 4 || size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return 0;
  }

  info.buffer = static_cast<const bfd_byte*>(code);
  info.buffer_vma = base;
  info.buffer_length = static_cast<int>(size);
  return decode_insn_ppc(base, &info, &out);
}

int Disassemble(const void* code, u64 base, ppc_insn* out, size_t nOut) {
  for (size_t i = 0; i < nOut; i++) {
    Disassemble(static_cast<const u32*>(code) + i, base, out[i]);
  }
  return static_cast<int>(nOut) * 4;
}

}  // namespace rex::codegen::ppc
