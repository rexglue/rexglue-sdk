#include "instruction.h"
#include "opcode.h"

#include <rex/types.h>

namespace rex::codegen::detail {

namespace {

InstrFormat FormatOf(Opcode opcode) {
  switch (opcode) {
    case Opcode::b:
    case Opcode::ba:
    case Opcode::bl:
    case Opcode::bla:
      return InstrFormat::kI;
    case Opcode::bc:
    case Opcode::bca:
    case Opcode::bcl:
    case Opcode::bcla:
      return InstrFormat::kB;
    case Opcode::bclr:
    case Opcode::bclrl:
    case Opcode::bcctr:
    case Opcode::bcctrl:
      return InstrFormat::kXL;
    default:
      return InstrFormat::kUnknown;
  }
}

}  // namespace

bool Instruction::IsBranch() const {
  switch (opcode) {
    case Opcode::b:
    case Opcode::ba:
    case Opcode::bl:
    case Opcode::bla:
    case Opcode::bc:
    case Opcode::bca:
    case Opcode::bcl:
    case Opcode::bcla:
    case Opcode::bclr:
    case Opcode::bclrl:
    case Opcode::bcctr:
    case Opcode::bcctrl:
      return true;
    default:
      return false;
  }
}

bool Instruction::IsCall() const {
  switch (opcode) {
    case Opcode::bl:
    case Opcode::bla:
    case Opcode::bcl:
    case Opcode::bcla:
    case Opcode::bclrl:
    case Opcode::bcctrl:
      return true;
    default:
      return false;
  }
}

bool Instruction::IsReturn() const {
  // blr is bclr with BO=20, the unconditional form
  if (opcode == Opcode::bclr) {
    return (raw == 0x4E800020);
  }
  return false;
}

bool Instruction::IsConditional() const {
  switch (opcode) {
    case Opcode::bc:
    case Opcode::bca:
    case Opcode::bcl:
    case Opcode::bcla:
      return true;
    case Opcode::bclr:
    case Opcode::bclrl:
    case Opcode::bcctr:
    case Opcode::bcctrl:
      // BO=20 branches always
      return XL.BO != 20;
    default:
      return false;
  }
}

Instruction DecodeInstruction(GuestAddrT address, u32 code) {
  Instruction instr;
  instr.address = address;
  instr.code = code;
  instr.raw = code;

  instr.opcode = LookupOpcode(code);
  instr.format = FormatOf(instr.opcode);

  if (instr.IsBranch()) {
    switch (instr.format) {
      case InstrFormat::kI: {
        i32 offset = Instruction::GetIOffset(code);
        bool aa = (code >> 1) & 1;
        if (aa) {
          instr.branch_target = static_cast<GuestAddrT>(offset & 0xFFFFFFFF);
        } else {
          instr.branch_target = address + offset;
        }
        break;
      }

      case InstrFormat::kB: {
        i32 offset = Instruction::GetBOffset(code);
        bool aa = (code >> 1) & 1;
        if (aa) {
          instr.branch_target = static_cast<GuestAddrT>(offset & 0xFFFF);
        } else {
          instr.branch_target = address + offset;
        }
        break;
      }

      case InstrFormat::kXL: {
        // The target is read from LR or CTR at run time
        instr.branch_target = std::nullopt;
        break;
      }

      default:
        instr.branch_target = std::nullopt;
        break;
    }
  }

  return instr;
}

}  // namespace rex::codegen::detail
