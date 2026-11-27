#include <csetjmp>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <rex/logging/log_category.h>
#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>

#include "ppc/kernel_hook.h"

namespace rex::ppc {

void Trap(PPCContext& ctx, u8* /*base*/, u16 trap_type) {
  switch (trap_type) {
    case 20:
    case 26: {
      detail::WriteDebugText(ctx.r3.u32, ctx.r4.u16);
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
