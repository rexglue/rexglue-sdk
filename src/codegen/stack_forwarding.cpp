#include "stack_forwarding.h"

#include <string_view>

#include <dis-asm.h>
#include <ppc-inst.h>

namespace rex::codegen {

namespace {

bool IsHeldWrite(int id) {
  switch (id) {
    case PPC_INST_STB:
    case PPC_INST_STH:
    case PPC_INST_STW:
    case PPC_INST_STD:
    case PPC_INST_STFS:
    case PPC_INST_STFD:
      return true;
    default:
      return false;
  }
}

bool StartsWith(std::string_view name, std::string_view prefix) {
  return name.starts_with(prefix);
}

bool WritesStackPointer(const ppc_insn& insn) {
  if (insn.operands[0] != 1) {
    return false;
  }
  const std::string_view text = insn.op_str;
  return text.starts_with("r1") && (text.size() == 2 || text[2] == ',');
}

}  // namespace

void StackForwarding::Invalidate() {
  for (Slot& slot : slots_) {
    slot.live = false;
  }
}

void StackForwarding::OnInstruction(const ppc_insn& insn, int id) {
  if (insn.opcode == nullptr) {
    Invalidate();
    return;
  }
  if (IsHeldWrite(id)) {
    if (insn.operands[2] != 1) {
      Invalidate();
    }
    return;
  }
  const std::string_view name = insn.opcode->name;
  if (StartsWith(name, "st") || StartsWith(name, "b") || StartsWith(name, "t") ||
      StartsWith(name, "sc") || StartsWith(name, "dcb") || StartsWith(name, "mtmsr") ||
      name.contains("sync") || name == "eieio" || name.ends_with("u") || name.ends_with("ux") ||
      WritesStackPointer(insn)) {
    Invalidate();
  }
}

u8 StackForwarding::Hold(i32 offset, u8 width) {
  for (Slot& slot : slots_) {
    if (slot.live && offset < slot.offset + slot.width && slot.offset < offset + width) {
      slot.live = false;
    }
  }
  const u8 index = next_;
  next_ = static_cast<u8>((next_ + 1) % kSlots);
  slots_[index] = Slot{offset, width, true};
  return index;
}

std::optional<u8> StackForwarding::Find(i32 offset, u8 width) const {
  for (size_t i = 0; i < kSlots; ++i) {
    if (slots_[i].live && slots_[i].offset == offset && slots_[i].width == width) {
      return static_cast<u8>(i);
    }
  }
  return std::nullopt;
}

}  // namespace rex::codegen
