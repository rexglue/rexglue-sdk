/**
 * @file        arch/ppc/instruction.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "instruction.h"
#include "opcode.h"

#include <rex/types.h>

namespace rex::codegen::ppc {

//=============================================================================
// Helper functions
//=============================================================================

// Special SPR numbers
constexpr u32 SPR_LR = 8;   // Link Register
constexpr u32 SPR_CTR = 9;  // Count Register

//=============================================================================
// Instruction helper methods implementation
//=============================================================================

bool Instruction::is_branch() const {
  const auto& info = get_opcode_info(opcode);
  return info.group == OpcodeGroup::kBranch;
}

bool Instruction::is_call() const {
  switch (opcode) {
    case Opcode::bl:
    case Opcode::bla:
      return true;
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
  // blr is bclr with BO=20 (unconditional)
  if (opcode == Opcode::bclr) {
    return (raw == 0x4E800020);  // Specific encoding for blr
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
      // Check BO field - unconditional if BO=20
      return XL.BO != 20;
    default:
      return false;
  }
}

//=============================================================================
// Instruction decoding
//=============================================================================

Instruction decode_instruction(guest_addr_t address, u32 code) {
  Instruction instr;
  instr.address = address;
  instr.code = code;
  instr.raw = code;  // Store in union

  // Lookup opcode
  instr.opcode = lookup_opcode(code);

  // Get opcode info
  const auto& info = get_opcode_info(instr.opcode);
  instr.format = info.format;

  // Compute branch target if applicable
  if (instr.is_branch()) {
    switch (instr.format) {
      case InstrFormat::kI: {
        // Unconditional branch (b, ba, bl, bla)
        // Use reliable XOR-subtract sign extension
        i32 offset = Instruction::get_i_offset(code);
        bool aa = (code >> 1) & 1;  // AA bit (absolute address)
        if (aa) {
          // Absolute address
          instr.branch_target = static_cast<guest_addr_t>(offset & 0xFFFFFFFF);
        } else {
          // Relative address
          instr.branch_target = address + offset;
        }
        break;
      }

      case InstrFormat::kB: {
        // Conditional branch (bc, bca, bcl, bcla)
        // Use reliable XOR-subtract sign extension
        i32 offset = Instruction::get_b_offset(code);
        bool aa = (code >> 1) & 1;  // AA bit (absolute address)
        if (aa) {
          // Absolute address
          instr.branch_target = static_cast<guest_addr_t>(offset & 0xFFFF);
        } else {
          // Relative address
          instr.branch_target = address + offset;
        }
        break;
      }

      case InstrFormat::kXL: {
        // Indirect branch (bclr, bcctr)
        // Target is runtime-dependent (in LR or CTR)
        instr.branch_target = std::nullopt;
        break;
      }

      default:
        instr.branch_target = std::nullopt;
        break;
    }
  }

  // Handle simplified mnemonics
  // blr = bclr with BO=20
  if (code == 0x4E800020) {
    instr.opcode = Opcode::bclr;  // Already set, but ensure
  }

  // bctr = bcctr with BO=20
  if (code == 0x4E800420) {
    instr.opcode = Opcode::bcctr;
  }

  // nop = ori 0,0,0
  if (code == 0x60000000) {
    instr.opcode = Opcode::nop;
  }

  // li rD, value = addi rD, 0, value
  if (instr.opcode == Opcode::addi && instr.D.RA == 0) {
    instr.opcode = Opcode::li;
  }

  // lis rD, value = addis rD, 0, value
  if (instr.opcode == Opcode::addis && instr.D.RA == 0) {
    instr.opcode = Opcode::lis;
  }

  // mr rA, rS = or rA, rS, rS (both source operands are the same register)
  if (instr.opcode == Opcode::or_ && instr.X.RT == instr.X.RB) {
    instr.opcode = Opcode::mr;
  }

  // mflr rD = mfspr rD, LR (SPR 8)
  if (instr.opcode == Opcode::mfspr && instr.XFX.spr_num() == SPR_LR) {
    instr.opcode = Opcode::mflr;
  }

  // mtlr rS = mtspr LR, rS (SPR 8)
  if (instr.opcode == Opcode::mtspr && instr.XFX.spr_num() == SPR_LR) {
    instr.opcode = Opcode::mtlr;
  }

  // mfctr rD = mfspr rD, CTR (SPR 9)
  if (instr.opcode == Opcode::mfspr && instr.XFX.spr_num() == SPR_CTR) {
    instr.opcode = Opcode::mfctr;
  }

  // mtctr rS = mtspr CTR, rS (SPR 9)
  if (instr.opcode == Opcode::mtspr && instr.XFX.spr_num() == SPR_CTR) {
    instr.opcode = Opcode::mtctr;
  }

  return instr;
}

std::string Instruction::to_string() const {
  // Placeholder - will be implemented in ppc_disasm.cpp
  const auto& info = get_opcode_info(opcode);
  return std::string(info.name);
}
}  // namespace rex::codegen::ppc
