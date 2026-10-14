#include "instruction.h"
#include "opcode.h"

#include <rex/types.h>

namespace rex::codegen::detail {

constexpr u32 kSPRLR = 8;
constexpr u32 kSPRCTR = 9;

bool Instruction::IsBranch() const {
  const auto& info = GetOpcodeInfo(opcode);
  return info.group == OpcodeGroup::kBranch;
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

  const auto& info = GetOpcodeInfo(instr.opcode);
  instr.format = info.format;

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

  if (code == 0x4E800020) {
    instr.opcode = Opcode::bclr;
  }

  if (code == 0x4E800420) {
    instr.opcode = Opcode::bcctr;
  }

  if (code == 0x60000000) {
    instr.opcode = Opcode::nop;
  }

  if (instr.opcode == Opcode::addi && instr.D.RA == 0) {
    instr.opcode = Opcode::li;
  }

  if (instr.opcode == Opcode::addis && instr.D.RA == 0) {
    instr.opcode = Opcode::lis;
  }

  if (instr.opcode == Opcode::or_ && instr.X.RT == instr.X.RB) {
    instr.opcode = Opcode::mr;
  }

  if (instr.opcode == Opcode::mfspr && instr.XFX.GetSPRNum() == kSPRLR) {
    instr.opcode = Opcode::mflr;
  }

  if (instr.opcode == Opcode::mtspr && instr.XFX.GetSPRNum() == kSPRLR) {
    instr.opcode = Opcode::mtlr;
  }

  if (instr.opcode == Opcode::mfspr && instr.XFX.GetSPRNum() == kSPRCTR) {
    instr.opcode = Opcode::mfctr;
  }

  if (instr.opcode == Opcode::mtspr && instr.XFX.GetSPRNum() == kSPRCTR) {
    instr.opcode = Opcode::mtctr;
  }

  return instr;
}

std::string Instruction::ToString() const {
  const auto& info = GetOpcodeInfo(opcode);
  return std::string(info.name);
}
}  // namespace rex::codegen::detail
