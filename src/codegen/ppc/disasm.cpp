#include "disasm.h"

#include <limits>

#include <rex/types.h>

namespace rex::codegen::detail {

thread_local DisassemblerEngine g_big_endian_disassembler{BFD_ENDIAN_BIG, nullptr};

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

}  // namespace rex::codegen::detail
