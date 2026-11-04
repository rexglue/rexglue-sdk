#include <csetjmp>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

#include <rex/logging/log.h>
#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>

namespace rex::ppc {

void Trap(PPCContext& ctx, u8* base, u16 trap_type) {
  switch (trap_type) {
    case 20:
    case 26: {
      const u32 address = ctx.r3.u32;
      const u32 offset = address >= 0xE0000000u ? 0x1000u : 0u;
      const std::string_view text(reinterpret_cast<const char*>(base + address + offset),
                                  ctx.r4.u16);
      REXCPU_DEBUG("(service trap) {}", text);
      break;
    }
    case 0:
    case 22:
      REXCPU_WARN("tw/td trap hit (type {})", trap_type);
      break;
    case 25:
      break;
    default:
      REXCPU_WARN("Unknown trap type {}", trap_type);
      break;
  }
}

namespace {
std::unordered_map<u32, std::jmp_buf>& JumpBuffers() {
  thread_local std::unordered_map<u32, std::jmp_buf> buffers;
  return buffers;
}
}  // namespace

std::jmp_buf& GetJumpBuffer(u32 address) {
  return JumpBuffers()[address];
}

void LongJump(u32 address, int value) {
  auto& buffers = JumpBuffers();
  const auto it = buffers.find(address);
  if (it != buffers.end()) {
    std::longjmp(it->second, value);
  }
  std::abort();
}

void UnimplementedInstruction(const char* opcode) {
  throw std::runtime_error(std::string("Unimplemented PPC instruction: ") + opcode);
}

}  // namespace rex::ppc
