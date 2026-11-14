#include "opcode.h"

#include <array>

#include <ppc.h>

#include "disasm.h"

extern "C" {
extern const powerpc_opcode powerpc_opcodes[];
extern const int powerpc_num_opcodes;
int AreOperandsValid(const powerpc_opcode* entry, unsigned long code, int dialect);
}

namespace rex::codegen::detail {

namespace {

/**
 * The binutils opcode table indexed by primary opcode.
 */
class OpcodeIndex {
 public:
  OpcodeIndex() {
    int entry = 0;
    for (u32 primary = 0; primary <= 64; ++primary) {
      while (entry < powerpc_num_opcodes && PPC_OP(powerpc_opcodes[entry].opcode) < primary) {
        ++entry;
      }
      start_[primary] = entry;
    }
  }

  const powerpc_opcode* Find(u32 code) const {
    const u32 primary = PPC_OP(code);
    for (int i = start_[primary]; i < start_[primary + 1]; ++i) {
      const powerpc_opcode& entry = powerpc_opcodes[i];
      if ((code & entry.mask) != entry.opcode || (entry.flags & kXenonDialect) == 0) {
        continue;
      }
      if (AreOperandsValid(&entry, code, static_cast<int>(kXenonDialect)) != 0) {
        return &entry;
      }
    }
    return nullptr;
  }

 private:
  std::array<int, 65> start_{};
};

const OpcodeIndex& GetIndex() {
  static const OpcodeIndex kIndex;
  return kIndex;
}

Opcode FromBranchEntry(const powerpc_opcode& entry) {
  const u32 base = static_cast<u32>(entry.opcode);
  const bool link = (base & 1) != 0;
  const bool absolute = (base & 2) != 0;
  switch (PPC_OP(base)) {
    case 16:
      if (link) {
        return absolute ? Opcode::bcla : Opcode::bcl;
      }
      return absolute ? Opcode::bca : Opcode::bc;
    case 18:
      if (link) {
        return absolute ? Opcode::bla : Opcode::bl;
      }
      return absolute ? Opcode::ba : Opcode::b;
    case 19:
      if (PPC_XOP(base) == 16) {
        return link ? Opcode::bclrl : Opcode::bclr;
      }
      if (PPC_XOP(base) == 528) {
        return link ? Opcode::bcctrl : Opcode::bcctr;
      }
      return Opcode::kOther;
    default:
      return Opcode::kOther;
  }
}

Opcode FromPrimary31(u32 xop) {
  switch (xop) {
    case 4:
      return Opcode::tw;
    case 23:
      return Opcode::lwzx;
    case 24:
      return Opcode::slw;
    case 28:
      return Opcode::and_;
    case 40:
    case 552:
      return Opcode::subf;
    case 87:
      return Opcode::lbzx;
    case 266:
    case 778:
      return Opcode::add;
    case 279:
      return Opcode::lhzx;
    case 316:
      return Opcode::xor_;
    case 339:
      return Opcode::mfspr;
    case 444:
      return Opcode::or_;
    case 467:
      return Opcode::mtspr;
    case 536:
      return Opcode::srw;
    case 792:
      return Opcode::sraw;
    case 824:
      return Opcode::srawi;
    default:
      return Opcode::kOther;
  }
}

}  // namespace

namespace {
constexpr std::array<int, 22> kDroppedIDs = {
    PPC_INST_VMULOUB,    PPC_INST_VMULOUH,  PPC_INST_VMULOSB,  PPC_INST_VMULOSH,
    PPC_INST_VMULEUB,    PPC_INST_VMULEUH,  PPC_INST_VMULESB,  PPC_INST_VMULESH,
    PPC_INST_VMLADDUHM,  PPC_INST_VMSUMUBM, PPC_INST_VMSUMMBM, PPC_INST_VMSUMUHM,
    PPC_INST_VMSUMUHS,   PPC_INST_VMSUMSHM, PPC_INST_VMSUMSHS, PPC_INST_VMHADDSHS,
    PPC_INST_VMHRADDSHS, PPC_INST_VSUM4UBS, PPC_INST_VSUM4SBS, PPC_INST_VSUM4SHS,
    PPC_INST_VSUM2SWS,   PPC_INST_VSUMSWS};
}  // namespace

bool IsInvalidOnXenon(int id) {
  for (const int dropped : kDroppedIDs) {
    if (dropped == id) {
      return true;
    }
  }
  return false;
}

int ResolveID(const ppc_insn& insn) {
  if (insn.opcode == nullptr) {
    return 0;
  }
  const int id = insn.opcode->id;
  if (IsInvalidOnXenon(id)) {
    return 0;
  }
  if (id == PPC_INST_VUPKHSB128 && insn.operands[2] == 0x60) {
    return PPC_INST_VUPKHSH128;
  }
  if (id == PPC_INST_VUPKLSB128 && insn.operands[2] == 0x60) {
    return PPC_INST_VUPKLSH128;
  }
  return id;
}

Opcode OpcodeOf(const powerpc_opcode* entry) {
  if (entry == nullptr || IsInvalidOnXenon(entry->id)) {
    return Opcode::kUnknown;
  }
  switch (entry->id) {
    case PPC_INST_NOP:
      return Opcode::nop;
    case PPC_INST_LI:
    case PPC_INST_LIL:
      return Opcode::li;
    case PPC_INST_LIS:
    case PPC_INST_LIU:
      return Opcode::lis;
    case PPC_INST_MR:
      return Opcode::mr;
    case PPC_INST_MFLR:
      return Opcode::mflr;
    case PPC_INST_MTLR:
      return Opcode::mtlr;
    case PPC_INST_MFCTR:
      return Opcode::mfctr;
    case PPC_INST_MTCTR:
      return Opcode::mtctr;
    case PPC_INST_SC:
      return Opcode::sc;
    default:
      break;
  }
  const u32 base = static_cast<u32>(entry->opcode);
  switch (PPC_OP(base)) {
    case 3:
      return Opcode::twi;
    case 10:
      return Opcode::cmpli;
    case 11:
      return Opcode::cmpi;
    case 14:
      return Opcode::addi;
    case 15:
      return Opcode::addis;
    case 16:
    case 18:
    case 19:
      return FromBranchEntry(*entry);
    case 21:
      return Opcode::rlwinm;
    case 24:
      return Opcode::ori;
    case 31:
      return FromPrimary31(PPC_XOP(base));
    case 32:
      return Opcode::lwz;
    case 34:
      return Opcode::lbz;
    case 37:
      return Opcode::stwu;
    case 40:
      return Opcode::lhz;
    default:
      return Opcode::kOther;
  }
}

namespace {

void DecodeWord(u32 code, u64 address, ppc_insn& out) {
  const std::array<u8, 4> bytes = {static_cast<u8>(code >> 24), static_cast<u8>(code >> 16),
                                   static_cast<u8>(code >> 8), static_cast<u8>(code)};
  out = ppc_insn{};
  Disassemble(bytes.data(), 4, address, out);
}

}  // namespace

int LookupID(u32 code) {
  const powerpc_opcode* entry = GetIndex().Find(code);
  if (entry == nullptr || IsInvalidOnXenon(entry->id)) {
    return 0;
  }
  if (entry->id == PPC_INST_VUPKHSB128 || entry->id == PPC_INST_VUPKLSB128) {
    ppc_insn insn;
    DecodeWord(code, 0, insn);
    return ResolveID(insn);
  }
  return entry->id;
}

Opcode LookupOpcode(u32 code) {
  return OpcodeOf(GetIndex().Find(code));
}

}  // namespace rex::codegen::detail
