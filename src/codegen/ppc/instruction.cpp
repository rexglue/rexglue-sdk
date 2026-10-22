#include "instruction.h"
#include "disasm.h"
#include "opcode.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <string_view>

#include <rex/types.h>

namespace rex::codegen::detail {

namespace {

constexpr u32 kIgnoreBranchConditions = 0x14;

constexpr int kClassGPR = 1;
constexpr int kClassGPROrZero = 2;
constexpr int kClassFPR = 3;
constexpr u32 kRegisterCount = 32;

constexpr u32 RegisterRange(u32 first, u32 last) {
  u32 mask = 0;
  for (u32 index = first; index <= last; ++index) {
    mask |= 1u << index;
  }
  return mask;
}

bool ReadsEveryOperand(std::string_view name) {
  constexpr std::array<std::string_view, 11> kPrefixes = {"st",  "cmp", "fcmp", "tw",  "td", "mt",
                                                          "dcb", "icb", "dst",  "dss", "eco"};
  return std::ranges::any_of(kPrefixes,
                             [&](std::string_view prefix) { return name.starts_with(prefix); });
}

bool InsertsIntoDestination(std::string_view name) {
  if (name.ends_with('.')) {
    name.remove_suffix(1);
  }
  return name == "rlwimi" || name == "rldimi" || name == "inslwi" || name == "insrwi" ||
         name == "insrdi";
}

bool IsUpdateForm(std::string_view name) {
  if (!name.starts_with('l') && !name.starts_with("st")) {
    return false;
  }
  return name != "liu" && (name.ends_with("ux") || name.ends_with('u'));
}

u32 WordCount(const ppc_insn& insn, std::string_view name) {
  if (name == "lmw" || name == "stmw") {
    return kRegisterCount - insn.operands[0];
  }
  if (name == "lswi" || name == "stswi") {
    const u32 bytes = insn.operands[2] == 0 ? 32 : insn.operands[2];
    return (bytes + 3) / 4;
  }
  return 1;
}

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
      return (XL.BO & kIgnoreBranchConditions) != kIgnoreBranchConditions;
    default:
      return false;
  }
}

bool Instruction::IsBranchAlways() const {
  switch (opcode) {
    case Opcode::b:
    case Opcode::ba:
      return true;
    case Opcode::bc:
    case Opcode::bca:
      return (B.BO & kIgnoreBranchConditions) == kIgnoreBranchConditions;
    case Opcode::bclr:
    case Opcode::bcctr:
      return (XL.BO & kIgnoreBranchConditions) == kIgnoreBranchConditions;
    default:
      return false;
  }
}

bool Instruction::IsTrapAlways() const {
  constexpr u32 kTrapAlways = 31;
  if (opcode == Opcode::twi)
    return D.RT == kTrapAlways;
  return opcode == Opcode::tw && X.RT == kTrapAlways;
}

Instruction DecodeInstruction(u32 address, u32 code) {
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
          instr.branch_target = static_cast<u32>(offset & 0xFFFFFFFF);
        } else {
          instr.branch_target = address + offset;
        }
        break;
      }

      case InstrFormat::kB: {
        i32 offset = Instruction::GetBOffset(code);
        bool aa = (code >> 1) & 1;
        if (aa) {
          instr.branch_target = static_cast<u32>(offset & 0xFFFF);
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

RegisterEffects GetRegisterEffects(const Instruction& instr) {
  RegisterEffects effects;
  if (instr.IsCall()) {
    effects.gpr_def = RegisterRange(3, 12);
    effects.fpr_def = RegisterRange(0, 13);
    return effects;
  }

  ppc_insn insn{};
  Disassemble(&instr.code, 4, instr.address, insn);
  if (insn.opcode == nullptr) {
    return effects;
  }

  const std::string_view name = insn.opcode->name;
  const bool reads_every_operand = ReadsEveryOperand(name);
  const bool is_update_form = IsUpdateForm(name);
  const unsigned update_position =
      is_update_form && (OperandRegisterClass(insn.opcode, 1) == kClassGPR ||
                         OperandRegisterClass(insn.opcode, 1) == kClassGPROrZero)
          ? 1
          : 2;
  bool first_register = true;
  bool first_gpr = true;
  for (unsigned position = 0; position < std::size(insn.opcode->operands); ++position) {
    if (insn.opcode->operands[position] == 0) {
      break;
    }
    const int register_class = OperandRegisterClass(insn.opcode, position);
    const u32 number = insn.operands[position];
    if (register_class == 0 || number >= kRegisterCount ||
        (register_class == kClassGPROrZero && number == 0)) {
      continue;
    }
    const bool is_destination = first_register && !reads_every_operand;
    first_register = false;
    if (register_class == kClassFPR) {
      (is_destination ? effects.fpr_def : effects.fpr_use) |= 1u << number;
    } else if (register_class == kClassGPR || register_class == kClassGPROrZero) {
      const u32 count = first_gpr ? WordCount(insn, name) : 1;
      const u32 bits = RegisterRange(number, std::min(number + count - 1, kRegisterCount - 1));
      (is_destination ? effects.gpr_def : effects.gpr_use) |= bits;
      if (is_destination && InsertsIntoDestination(name)) {
        effects.gpr_use |= bits;
      }
      if (is_update_form && position == update_position) {
        effects.gpr_def |= bits;
      }
      first_gpr = false;
    }
  }
  return effects;
}

bool IsFrameAllocation(const Instruction& instr) {
  const u32 word = static_cast<u32>(instr.code);
  const u32 primary = word >> 26;
  const u32 base = (word >> 16) & 31;
  if (base != 1) {
    return false;
  }
  return primary == 37 || (primary == 62 && (word & 3) == 1);
}

bool IsCallerFrameSpill(const Instruction& instr) {
  const u32 word = static_cast<u32>(instr.code);
  const u32 primary = word >> 26;
  const u32 base = (word >> 16) & 31;
  if (base != 1) {
    return false;
  }
  switch (primary) {
    case 36:
    case 38:
    case 44:
    case 52:
    case 54:
      return static_cast<i16>(word & 0xFFFF) > 0;
    case 62:
      return (word & 3) == 0 && static_cast<i16>(word & 0xFFFC) > 0;
    default:
      return false;
  }
}

}  // namespace rex::codegen::detail
