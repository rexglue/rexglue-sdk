#include "instruction.h"
#include "opcode.h"

#include <rex/types.h>

namespace rex::codegen::ppc {

constexpr u32 SPR_LR = 8;
constexpr u32 SPR_CTR = 9;

bool Instruction::is_branch() const {
  const auto& info = get_opcode_info(opcode);
  return info.group == OpcodeGroup::kBranch;
}

bool Instruction::is_call() const {
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

bool Instruction::is_return() const {
  // blr is bclr with BO=20, the unconditional form
  if (opcode == Opcode::bclr) {
    return (raw == 0x4E800020);
  }
  return false;
}

bool Instruction::is_conditional() const {
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

Instruction decode_instruction(guest_addr_t address, u32 code) {
  Instruction instr;
  instr.address = address;
  instr.code = code;
  instr.raw = code;

  instr.opcode = lookup_opcode(code);

  const auto& info = get_opcode_info(instr.opcode);
  instr.format = info.format;

  if (instr.is_branch()) {
    switch (instr.format) {
      case InstrFormat::kI: {
        i32 offset = Instruction::get_i_offset(code);
        bool aa = (code >> 1) & 1;
        if (aa) {
          instr.branch_target = static_cast<guest_addr_t>(offset & 0xFFFFFFFF);
        } else {
          instr.branch_target = address + offset;
        }
        break;
      }

      case InstrFormat::kB: {
        i32 offset = Instruction::get_b_offset(code);
        bool aa = (code >> 1) & 1;
        if (aa) {
          instr.branch_target = static_cast<guest_addr_t>(offset & 0xFFFF);
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

  if (instr.opcode == Opcode::mfspr && instr.XFX.spr_num() == SPR_LR) {
    instr.opcode = Opcode::mflr;
  }

  if (instr.opcode == Opcode::mtspr && instr.XFX.spr_num() == SPR_LR) {
    instr.opcode = Opcode::mtlr;
  }

  if (instr.opcode == Opcode::mfspr && instr.XFX.spr_num() == SPR_CTR) {
    instr.opcode = Opcode::mfctr;
  }

  if (instr.opcode == Opcode::mtspr && instr.XFX.spr_num() == SPR_CTR) {
    instr.opcode = Opcode::mtctr;
  }

  return instr;
}

std::string Instruction::to_string() const {
  const auto& info = get_opcode_info(opcode);
  return std::string(info.name);
}
}  // namespace rex::codegen::ppc
