#include "decoded_binary.h"
#include "ppc/instruction.h"
#include "ppc/opcode.h"

#include <algorithm>
#include <queue>
#include <set>
#include <stack>
#include <unordered_set>

#include <format>

#include <rex/codegen/binary_view.h>
#include <rex/codegen/codegen_context.h>
#include <rex/codegen/function_scanner.h>
#include <rex/logging/log.h>

#include <rex/memory/byte_swap.h>
#include <rex/types.h>

namespace rex::codegen {

using rex::codegen::detail::DecodeInstruction;
using rex::codegen::detail::Instruction;
using rex::codegen::detail::Opcode;

FunctionScanner::FunctionScanner(const BinaryView& binary, const Limits& limits)
    : binary_(&binary), limits_(&limits) {}

template <typename T>
const T* FunctionScanner::TranslateAddress(rex::GuestAddrT guest_addr) const {
  return reinterpret_cast<const T*>(binary_->Translate(static_cast<u32>(guest_addr)));
}

bool FunctionScanner::IsExecutableSection(rex::GuestAddrT address) const {
  return binary_->IsExecutable(static_cast<u32>(address));
}

bool FunctionScanner::IsProloguePattern(GuestAddrT address) const {
  auto host_ptr = TranslateAddress<u32>(address);
  if (!host_ptr)
    return false;

  u32 code = memory::load<be_u32>(host_ptr);
  Instruction instr = DecodeInstruction(address, code);

  if (instr.opcode == Opcode::mflr) {
    return true;
  }

  if (instr.opcode == Opcode::mfspr && instr.XFX.GetSPRNum() == 8) {
    return true;
  }

  if (instr.opcode == Opcode::stwu && instr.D.RS() == 1 && instr.D.RA == 1 && instr.D.SIMM() < 0) {
    return true;
  }

  return false;
}

bool FunctionScanner::IsEpiloguePattern(GuestAddrT address) const {
  auto host_ptr = TranslateAddress<u32>(address);
  if (!host_ptr)
    return false;

  u32 code = memory::load<be_u32>(host_ptr);

  Instruction instr = DecodeInstruction(address, code);

  if (instr.IsReturn()) {
    return true;
  }

  if (instr.opcode == Opcode::mtlr) {
    return true;
  }

  if (instr.opcode == Opcode::lwz && instr.D.RT == 1 && instr.D.RA == 1 && instr.D.SIMM() == 0) {
    return true;
  }

  return false;
}

/**
 * Reports whether an address starts a register save or restore helper. A helper starts with
 * ld r14, -0x98(r1), std r14, -0x98(r1), lfd f14, -0x90(r12) or stfd f14, -0x90(r12), or with
 * li r11, -0x120 followed by lvx or stvx v14, r11, r12, or li r11, -0x400 followed by lvx128 or
 * stvx128 v64, r11, r12.
 *
 * @param address Address to test.
 * @return True when the address starts a helper.
 */
bool FunctionScanner::IsRestgprlrFunction(GuestAddrT address) const {
  auto host_ptr = TranslateAddress<u32>(address);
  if (!host_ptr)
    return false;

  u32 first = memory::load<be_u32>(host_ptr);

  constexpr u32 kRestGPRLR14 = 0xe9c1ff68;
  constexpr u32 kSaveGPRLR14 = 0xf9c1ff68;
  constexpr u32 kRestFPR14 = 0xc9ccff70;
  constexpr u32 kSaveFPR14 = 0xd9ccff70;

  if (first == kRestGPRLR14 || first == kSaveGPRLR14 || first == kRestFPR14 ||
      first == kSaveFPR14) {
    return true;
  }

  if (first == 0x3960fee0) {
    auto second_ptr = TranslateAddress<u32>(address + 4);
    if (second_ptr) {
      u32 second = memory::load<be_u32>(second_ptr);
      constexpr u32 kRestVMX14 = 0x7dcb60ce;
      constexpr u32 kSaveVMX14 = 0x7dcb61ce;
      if (second == kRestVMX14 || second == kSaveVMX14) {
        return true;
      }
    }
  }

  if (first == 0x3960fc00) {
    auto second_ptr = TranslateAddress<u32>(address + 4);
    if (second_ptr) {
      u32 second = memory::load<be_u32>(second_ptr);
      constexpr u32 kRestVMX64 = 0x100b60cb;
      constexpr u32 kSaveVMX64 = 0x100b61cb;
      if (second == kRestVMX64 || second == kSaveVMX64) {
        return true;
      }
    }
  }

  return false;
}

namespace {

enum class JumpTableType : u8 {
  kAbsolute,
  kComputed,
  kByteOffset,
  kShortOffset,
};

struct JumpTableMatch {
  JumpTableType type = JumpTableType::kAbsolute;
  u8 ctr_source_reg = 0;
  u8 table_reg = 0;
  u8 base_reg = 0;
  u8 index_reg = 0;
  u8 offset_reg = 0;
  u8 shift_amount = 0;
  GuestAddrT table_high = 0;
  GuestAddrT table_low = 0;
  GuestAddrT base_high = 0;
  GuestAddrT base_low = 0;

  bool found_mtctr = false;
  bool found_add = false;
  bool found_load = false;
  bool found_shift = false;
  bool found_table_lis = false;
  bool found_table_addi = false;
  bool found_base_lis = false;
  bool found_base_addi = false;

  GuestAddrT GetTableAddress() const { return table_high + static_cast<i16>(table_low); }
  GuestAddrT GetBaseAddress() const { return base_high + static_cast<i16>(base_low); }
};

struct BoundsInfo {
  u32 max_entries = 0;
  u8 index_reg = 0;
  u32 default_target = 0;
  bool found = false;
};

/**
 * Scans up to 64 instructions back from a bctr for the bounds of its index register. A bc, bca,
 * bcl, bcla, bclr or bclrl testing the GT bit (BI mod 4 == 1, as bgt with BO 12 or ble with BO 4)
 * names the CR field and the default target. A clrlwi (rlwinm with SH 0, ME 31 and MB above 0)
 * implies 2^(32-MB) entries and is final when that is 2 to 256. A cmpli or cmpi gives its
 * immediate plus 1 entries, matched by register or by CR field, and an immediate of 0 or 1 is
 * rejected as an unrelated comparison.
 *
 * @param scanner Scanner used to read instructions.
 * @param bctr_address Address of the bctr.
 * @param expected_index_reg Register holding the switch index.
 * @return The bounds, with found set when one was seen.
 */
BoundsInfo ScanForBounds(const FunctionScanner& scanner, GuestAddrT bctr_address,
                         u8 expected_index_reg) {
  BoundsInfo bounds;
  constexpr int kMaxScan = 64;

  u8 cr_field = 0xFF;

  for (int i = 1; i <= kMaxScan; ++i) {
    GuestAddrT addr = bctr_address - (i * 4);
    if (addr < 4)
      break;

    auto host_ptr = scanner.TranslateAddress<u32>(addr);
    if (!host_ptr)
      break;

    u32 code = memory::load<be_u32>(host_ptr);
    if (code == 0)
      break;

    Instruction instr = DecodeInstruction(addr, code);

    if (cr_field == 0xFF) {
      bool is_cond_branch = (instr.opcode == Opcode::bc || instr.opcode == Opcode::bca ||
                             instr.opcode == Opcode::bcl || instr.opcode == Opcode::bcla ||
                             instr.opcode == Opcode::bclr || instr.opcode == Opcode::bclrl);

      if (is_cond_branch) {
        u8 bi = instr.B.BI;

        if ((bi & 0x3) == 1) {
          cr_field = (bi >> 2) & 0x7;

          if (instr.branch_target.has_value()) {
            bounds.default_target = instr.branch_target.value();
          }
        }
      }
    }

    if (instr.opcode == Opcode::rlwinm && instr.M.RA == expected_index_reg) {
      u8 sh = instr.M.SH;
      u8 mb = instr.M.MB;
      u8 me = instr.M.ME;

      if (sh == 0 && me == 31 && mb > 0 && mb < 32) {
        u32 implicit_count = 1u << (32 - mb);
        REXLOG_TRACE(codegen,
                     "  [0x{:08X}] Found clrlwi/rlwinm r{}, ..., {} -> {} entries (implicit mask)",
                     addr, static_cast<u32>(expected_index_reg), mb, implicit_count);

        if (implicit_count >= 2 && implicit_count <= 256) {
          bounds.max_entries = implicit_count;
          bounds.index_reg = expected_index_reg;
          bounds.found = true;
          break;
        }
      }
    }

    if (instr.opcode == Opcode::cmpli || instr.opcode == Opcode::cmpi) {
      u8 cmp_cr = instr.D.RT >> 2;
      u8 cmp_ra = instr.D.RA;
      u16 cmp_imm = instr.D.UIMM();

      bool cr_matches = (cr_field != 0xFF && cmp_cr == cr_field);
      bool reg_matches = (cmp_ra == expected_index_reg);

      if (cmp_imm <= 1) {
        REXLOG_TRACE(codegen,
                     "  [0x{:08X}] Skipping cmpli r{}, {} (immediate too small for switch bounds)",
                     addr, static_cast<u32>(cmp_ra), cmp_imm);
        continue;
      }

      if (reg_matches || (cr_matches && cmp_imm > 1)) {
        bounds.max_entries = cmp_imm + 1;
        bounds.index_reg = cmp_ra;
        bounds.found = true;

        if (reg_matches)
          break;
      }
    }
  }

  return bounds;
}

/**
 * Reads jump table targets as big endian entries. An absolute entry is 4 bytes, a computed entry
 * is 1 byte shifted and added to the base, a byte offset entry is 1 byte added to the base and a
 * short offset entry is 2 bytes added to the base. An entry count of 0 reads until an invalid
 * entry. Reading stops at a misaligned target or a null or non executable absolute target. A non
 * executable offset target is skipped.
 *
 * @param scanner Scanner used to read memory.
 * @param match Matched table layout.
 * @param entry_count Entry count from the bounds check, or 0 when unknown.
 * @return The targets.
 */
std::vector<GuestAddrT> ReadTableEntries(const FunctionScanner& scanner,
                                         const JumpTableMatch& match, u32 entry_count) {
  std::vector<GuestAddrT> targets;
  GuestAddrT table_addr = match.GetTableAddress();

  for (u32 i = 0; entry_count == 0 || i < entry_count; ++i) {
    GuestAddrT target = 0;

    switch (match.type) {
      case JumpTableType::kAbsolute: {
        auto entry_ptr = scanner.TranslateAddress<u32>(table_addr + (i * 4));
        if (!entry_ptr)
          goto done;
        target = memory::load<be_u32>(entry_ptr);
        break;
      }

      case JumpTableType::kComputed: {
        auto entry_ptr = scanner.TranslateAddress<u8>(table_addr + i);
        if (!entry_ptr)
          goto done;
        u8 offset = *entry_ptr;
        target = match.GetBaseAddress() + (static_cast<u32>(offset) << match.shift_amount);
        break;
      }

      case JumpTableType::kByteOffset: {
        auto entry_ptr = scanner.TranslateAddress<u8>(table_addr + i);
        if (!entry_ptr)
          goto done;
        u8 offset = *entry_ptr;
        target = match.GetBaseAddress() + offset;
        break;
      }

      case JumpTableType::kShortOffset: {
        auto entry_ptr = scanner.TranslateAddress<u16>(table_addr + (i * 2));
        if (!entry_ptr)
          goto done;
        u16 offset = memory::load<be_u16>(entry_ptr);
        target = match.GetBaseAddress() + offset;
        break;
      }
    }

    if (target & 3)
      goto done;

    if (target == 0 && match.type == JumpTableType::kAbsolute)
      goto done;

    if (!scanner.IsExecutableSection(target)) {
      if (match.type == JumpTableType::kAbsolute)
        goto done;
      continue;
    }

    targets.push_back(target);
  }

done:
  return targets;
}

}  // anonymous namespace

/**
 * Reports whether an instruction crosses into another function. Zero padding, blr, bctr, bctrl,
 * b, ba, mflr and stwu r1 are boundaries. A single nop is not.
 *
 * @param code Raw instruction word.
 * @param instr Decoded instruction.
 * @param addr Instruction address.
 * @return True when the instruction is a boundary.
 */
static bool IsFunctionBoundary(u32 code, const Instruction& instr, GuestAddrT addr) {
  if (code == 0x00000000) {
    REXLOG_TRACE(codegen, "  [0x{:08X}] Hit zero padding - function boundary", addr);
    return true;
  }

  if (instr.IsReturn()) {
    REXLOG_TRACE(codegen, "  [0x{:08X}] Hit blr - function boundary", addr);
    return true;
  }

  if (instr.opcode == Opcode::bcctr || instr.opcode == Opcode::bcctrl) {
    REXLOG_TRACE(codegen, "  [0x{:08X}] Hit bctr/bctrl - function boundary", addr);
    return true;
  }

  if (instr.opcode == Opcode::b || instr.opcode == Opcode::ba) {
    REXLOG_TRACE(codegen, "  [0x{:08X}] Hit unconditional branch (b) - function boundary", addr);
    return true;
  }

  if (instr.opcode == Opcode::mflr) {
    REXLOG_TRACE(codegen, "  [0x{:08X}] Hit mflr - function prologue", addr);
    return true;
  }

  if (instr.opcode == Opcode::stwu && instr.D.RA == 1 && instr.D.RT == 1) {
    REXLOG_TRACE(codegen, "  [0x{:08X}] Hit stwu r1 (stack frame) - function prologue", addr);
    return true;
  }

  return false;
}

/**
 * Detects a jump table by scanning back from a bctr. A bctr with a configured switch table is
 * skipped. The compiler emits 4 layouts.
 * - Absolute, lis and addi of the table, rlwinm index by 2, lwzx of full 32 bit targets, mtctr.
 * - Computed, lis and addi of the table, lbzx of a byte offset, rlwinm shift, lis and addi of the
 *   base, add, mtctr.
 * - Byte offset, as computed without the rlwinm shift.
 * - Short offset, as computed with the index scaled by 1 and lhzx of a 16 bit offset.
 *
 * Scanning continues past a bctr when a load was found without its lis, as adjacent tables share
 * setup code. When 1 register holds both table and base the scan finds the base first. An RA and
 * RB swap is corrected when a lis matches the index. A load with no lis is a vtable or indirect
 * call. The table needs at least 2 entries.
 *
 * @param bctr_address Address of the bctr.
 * @return The table, or nullopt when none matches.
 */
std::optional<JumpTable> FunctionScanner::DetectJumpTable(GuestAddrT bctr_address) {
  if (known_switch_tables_.count(static_cast<u32>(bctr_address))) {
    REXLOG_TRACE(codegen, "detect_jump_table: skipping 0x{:08X} (manual table exists)",
                 bctr_address);
    return std::nullopt;
  }

  const int max_scan_back = static_cast<int>(limits_->backward_scan_limit);

  JumpTableMatch match;

  REXLOG_TRACE(codegen, "detect_jump_table: scanning backward from bctr at 0x{:08X}", bctr_address);

  for (int i = 1; i <= max_scan_back; ++i) {
    GuestAddrT addr = bctr_address - (i * 4);
    if (addr < 4)
      break;

    auto host_ptr = TranslateAddress<u32>(addr);
    if (!host_ptr)
      break;

    u32 code = memory::load<be_u32>(host_ptr);

    Instruction instr = DecodeInstruction(addr, code);

    if (IsFunctionBoundary(code, instr, addr)) {
      if (instr.opcode == Opcode::bcctr && match.found_load && !match.found_table_lis) {
        REXLOG_TRACE(codegen, "  [0x{:08X}] Continuing past bctr to find shared lis", addr);
        continue;
      }
      break;
    }

    if (!match.found_mtctr && instr.opcode == Opcode::mtctr) {
      match.ctr_source_reg = instr.XFX.RS();
      match.found_mtctr = true;
      REXLOG_TRACE(codegen, "  [0x{:08X}] Found mtctr r{}", addr,
                   static_cast<u32>(match.ctr_source_reg));
      continue;
    }

    if (!match.found_mtctr)
      continue;

    if (!match.found_add && !match.found_load && instr.opcode == Opcode::add) {
      if (instr.XO.RT == match.ctr_source_reg) {
        match.base_reg = instr.XO.RA;
        match.offset_reg = instr.XO.RB;
        match.found_add = true;
        REXLOG_TRACE(codegen, "  [0x{:08X}] Found add r{}, r{}, r{}", addr,
                     static_cast<u32>(instr.XO.RT), static_cast<u32>(match.base_reg),
                     static_cast<u32>(match.offset_reg));
        continue;
      }
    }

    if (!match.found_add && !match.found_load && instr.opcode == Opcode::lwzx) {
      if (instr.X.RT == match.ctr_source_reg) {
        match.type = JumpTableType::kAbsolute;
        match.table_reg = instr.X.RA;
        match.index_reg = instr.X.RB;
        match.found_load = true;
        REXLOG_TRACE(codegen,
                     "  [0x{:08X}] Found lwzx r{}, r{}, r{} (tentative table=r{}, index=r{})", addr,
                     static_cast<u32>(instr.X.RT), static_cast<u32>(instr.X.RA),
                     static_cast<u32>(instr.X.RB), static_cast<u32>(match.table_reg),
                     static_cast<u32>(match.index_reg));
        continue;
      }
    }

    if (match.found_add && !match.found_load) {
      if (instr.opcode == Opcode::lbzx && instr.X.RT == match.offset_reg) {
        match.table_reg = instr.X.RA;
        match.index_reg = instr.X.RB;
        match.found_load = true;
        if (!match.found_shift) {
          match.type = JumpTableType::kByteOffset;
        }
        REXLOG_TRACE(codegen, "  [0x{:08X}] Found lbzx r{}, r{}, r{}", addr,
                     static_cast<u32>(instr.X.RT), static_cast<u32>(instr.X.RA),
                     static_cast<u32>(instr.X.RB));
        continue;
      }

      if (instr.opcode == Opcode::lhzx && instr.X.RT == match.offset_reg) {
        match.type = JumpTableType::kShortOffset;
        match.table_reg = instr.X.RA;
        match.index_reg = instr.X.RB;
        match.found_load = true;
        REXLOG_TRACE(codegen, "  [0x{:08X}] Found lhzx r{}, r{}, r{}", addr,
                     static_cast<u32>(instr.X.RT), static_cast<u32>(instr.X.RA),
                     static_cast<u32>(instr.X.RB));
        continue;
      }
    }

    if (!match.found_shift && instr.opcode == Opcode::rlwinm) {
      if (match.found_load && instr.M.RA == match.index_reg) {
        match.index_reg = instr.M.RS;
        match.found_shift = true;
        REXLOG_TRACE(codegen, "  [0x{:08X}] Found rlwinm (index scale) r{}, r{}, {}", addr,
                     static_cast<u32>(instr.M.RA), static_cast<u32>(instr.M.RS),
                     static_cast<u32>(instr.M.SH));
        continue;
      }
      if (match.found_add && instr.M.RA == match.offset_reg &&
          match.type != JumpTableType::kShortOffset) {
        match.shift_amount = instr.M.SH;
        match.type = JumpTableType::kComputed;
        match.found_shift = true;
        match.offset_reg = instr.M.RS;
        REXLOG_TRACE(codegen, "  [0x{:08X}] Found rlwinm (shift) r{}, r{}, {} -> COMPUTED type",
                     addr, static_cast<u32>(instr.M.RA), static_cast<u32>(instr.M.RS),
                     static_cast<u32>(instr.M.SH));
        continue;
      }
    }

    if (match.found_load) {
      bool register_reuse = match.found_add && (match.table_reg == match.base_reg);

      if (instr.opcode == Opcode::lis && instr.D.RT == match.table_reg) {
        if (register_reuse) {
          if (!match.found_base_lis) {
            match.base_high = static_cast<GuestAddrT>(instr.D.UIMM()) << 16;
            match.found_base_lis = true;
            REXLOG_TRACE(codegen, "  [0x{:08X}] Found base lis r{}, 0x{:04X} (register reuse)",
                         addr, static_cast<u32>(instr.D.RT), instr.D.UIMM());
            continue;
          } else if (!match.found_table_lis) {
            match.table_high = static_cast<GuestAddrT>(instr.D.UIMM()) << 16;
            match.found_table_lis = true;
            REXLOG_TRACE(codegen, "  [0x{:08X}] Found table lis r{}, 0x{:04X} (register reuse)",
                         addr, static_cast<u32>(instr.D.RT), instr.D.UIMM());
            continue;
          }
        } else if (!match.found_table_lis) {
          match.table_high = static_cast<GuestAddrT>(instr.D.UIMM()) << 16;
          match.found_table_lis = true;
          REXLOG_TRACE(codegen, "  [0x{:08X}] Found lis r{}, 0x{:04X}", addr,
                       static_cast<u32>(instr.D.RT), instr.D.UIMM());
          continue;
        }
      } else if (!match.found_table_lis && instr.opcode == Opcode::lis &&
                 instr.D.RT == match.index_reg) {
        REXLOG_TRACE(codegen, "  [0x{:08X}] Found lis for index_reg r{}, swapping table/index",
                     addr, static_cast<u32>(instr.D.RT));
        std::swap(match.table_reg, match.index_reg);
        match.table_high = static_cast<GuestAddrT>(instr.D.UIMM()) << 16;
        match.found_table_lis = true;
        continue;
      }

      if ((instr.opcode == Opcode::addi || instr.opcode == Opcode::ori) &&
          instr.D.RT == match.table_reg) {
        if (register_reuse) {
          if (!match.found_base_addi) {
            match.base_low = instr.D.UIMM();
            match.found_base_addi = true;
            REXLOG_TRACE(codegen, "  [0x{:08X}] Found base addi r{}, 0x{:04X} (register reuse)",
                         addr, static_cast<u32>(instr.D.RT), instr.D.UIMM());
            continue;
          } else if (!match.found_table_addi) {
            match.table_low = instr.D.UIMM();
            match.found_table_addi = true;
            REXLOG_TRACE(codegen, "  [0x{:08X}] Found table addi r{}, 0x{:04X} (register reuse)",
                         addr, static_cast<u32>(instr.D.RT), instr.D.UIMM());
            continue;
          }
        } else if (!match.found_table_addi) {
          match.table_low = instr.D.UIMM();
          match.found_table_addi = true;
          REXLOG_TRACE(codegen, "  [0x{:08X}] Found addi r{}, r{}, 0x{:04X}", addr,
                       static_cast<u32>(instr.D.RT), static_cast<u32>(instr.D.RA), instr.D.UIMM());
          continue;
        }
      } else if (!match.found_table_addi &&
                 (instr.opcode == Opcode::addi || instr.opcode == Opcode::ori) &&
                 instr.D.RT == match.index_reg) {
        REXLOG_TRACE(codegen, "  [0x{:08X}] Found addi for index_reg r{}, swapping table/index",
                     addr, static_cast<u32>(instr.D.RT));
        std::swap(match.table_reg, match.index_reg);
        match.table_low = instr.D.UIMM();
        match.found_table_addi = true;
        continue;
      }
    }

    if (match.found_add) {
      if (!match.found_base_lis && instr.opcode == Opcode::lis) {
        if (instr.D.RT == match.base_reg) {
          match.base_high = static_cast<GuestAddrT>(instr.D.UIMM()) << 16;
          match.found_base_lis = true;
          REXLOG_TRACE(codegen, "  [0x{:08X}] Found base lis r{}, 0x{:04X}", addr,
                       static_cast<u32>(instr.D.RT), instr.D.UIMM());
          continue;
        } else if (instr.D.RT == match.offset_reg && !match.found_base_lis) {
          REXLOG_TRACE(codegen, "  [0x{:08X}] Found lis for offset_reg r{}, swapping base/offset",
                       addr, static_cast<u32>(instr.D.RT));
          std::swap(match.base_reg, match.offset_reg);
          match.base_high = static_cast<GuestAddrT>(instr.D.UIMM()) << 16;
          match.found_base_lis = true;
          continue;
        }
      }
      if (!match.found_base_addi && (instr.opcode == Opcode::addi || instr.opcode == Opcode::ori)) {
        if (instr.D.RT == match.base_reg) {
          match.base_low = instr.D.UIMM();
          match.found_base_addi = true;
          REXLOG_TRACE(codegen, "  [0x{:08X}] Found base addi r{}, 0x{:04X}", addr,
                       static_cast<u32>(instr.D.RT), instr.D.UIMM());
          continue;
        } else if (instr.D.RT == match.offset_reg && !match.found_base_addi) {
          REXLOG_TRACE(codegen, "  [0x{:08X}] Found addi for offset_reg r{}, swapping base/offset",
                       addr, static_cast<u32>(instr.D.RT));
          std::swap(match.base_reg, match.offset_reg);
          match.base_low = instr.D.UIMM();
          match.found_base_addi = true;
          continue;
        }
      }
    }

    bool table_complete = match.found_table_lis && match.found_table_addi;
    bool base_complete = !match.found_add || (match.found_base_lis && match.found_base_addi);

    if (match.found_mtctr && match.found_load && table_complete && base_complete) {
      REXLOG_TRACE(codegen, "  Pattern complete at 0x{:08X}", addr);
      break;
    }
  }

  if (!match.found_mtctr || !match.found_load || !match.found_table_lis ||
      !match.found_table_addi) {
    REXLOG_TRACE(codegen, "  Pattern incomplete: mtctr={}, load={}, table_lis={}, table_addi={}",
                 match.found_mtctr, match.found_load, match.found_table_lis,
                 match.found_table_addi);
    if (match.found_load && (match.found_table_lis || match.found_base_lis)) {
      REXLOG_ERROR(codegen,
                   "Jump table detection failed at bctr 0x{:08X}: mtctr={}, load={}, table_lis={}, "
                   "table_addi={}, table_reg=r{}, base_lis={}, base_addi={}",
                   bctr_address, match.found_mtctr, match.found_load, match.found_table_lis,
                   match.found_table_addi, match.table_reg, match.found_base_lis,
                   match.found_base_addi);
    } else if (match.found_load) {
      REXLOG_TRACE(codegen,
                   "bctr 0x{:08X}: indexed load without lis - treating as vtable/indirect call",
                   bctr_address);
    }
    return std::nullopt;
  }

  if (match.found_add && (!match.found_base_lis || !match.found_base_addi)) {
    REXLOG_TRACE(codegen, "  Offset-based pattern incomplete: base_lis={}, base_addi={}",
                 match.found_base_lis, match.found_base_addi);
    return std::nullopt;
  }

  GuestAddrT table_address = match.GetTableAddress();
  REXLOG_TRACE(codegen, "  Table address: 0x{:08X} (high=0x{:08X}, low=0x{:04X})", table_address,
               match.table_high, match.table_low);

  auto table_ptr = TranslateAddress<u8>(table_address);
  if (!table_ptr) {
    REXLOG_TRACE(codegen, "  Invalid table address 0x{:08X} - not in mapped memory", table_address);
    return std::nullopt;
  }

  BoundsInfo bounds = ScanForBounds(*this, bctr_address, match.index_reg);
  REXLOG_TRACE(codegen, "  Bounds check: found={}, count={}, default=0x{:08X}, index_reg=r{}",
               bounds.found, bounds.max_entries, bounds.default_target, bounds.index_reg);

  std::vector<GuestAddrT> targets = ReadTableEntries(*this, match, bounds.max_entries);

  if (targets.size() < 2) {
    REXLOG_TRACE(codegen, "  Insufficient entries: {} (need at least 2)", targets.size());
    return std::nullopt;
  }

  JumpTable jt;
  jt.bctr_address = static_cast<u32>(bctr_address);
  jt.table_address = static_cast<u32>(table_address);
  jt.index_register = match.index_reg;
  jt.targets = std::move(targets);

  return jt;
}

/**
 * Discovers the blocks of a function depth first. A b is a tail call when its target is a known
 * callable, is below the entry, is in another code region and not a configured chunk, which stops
 * merges across null boundaries, or starts with a prologue. A conditional bclr ends a block with a
 * fall-through successor. The fall-through of a conditional branch is projected to the branch
 * target so it cannot grow past it.
 *
 * @param entry_point Function entry address.
 * @param pdata_size Function size from PDATA, or 0 when unknown.
 * @return The discovered blocks and calls.
 */
FunctionBlocks FunctionScanner::DiscoverBlocks(rex::GuestAddrT entry_point, rex::u32 pdata_size) {
  FunctionBlocks result;
  result.entry = entry_point;
  result.pdata_size = pdata_size;

  std::unordered_set<GuestAddrT> scanned_addrs;

  std::vector<DiscoveredBlock> block_stack;

  DiscoveredBlock entry_block;
  entry_block.base = entry_point;
  entry_block.end = entry_point;
  entry_block.projected_size = -1;
  block_stack.push_back(entry_block);

  const size_t max_blocks = limits_->max_blocks_per_function;

  while (!block_stack.empty() && result.blocks.size() < max_blocks) {
    DiscoveredBlock& block = block_stack.back();

    if (block.end == block.base) {
      if (scanned_addrs.count(block.base)) {
        block_stack.pop_back();
        continue;
      }
    }

    if ((block.base & 0x3) != 0) {
      REXLOG_WARN(codegen, "discover_blocks: misaligned block start 0x{:08X}", block.base);
      block_stack.pop_back();
      continue;
    }

    GuestAddrT addr = block.end;
    if (addr == block.base) {}

    GuestAddrT block_size = addr - block.base;
    if (block.projected_size != -1 && block_size >= static_cast<GuestAddrT>(block.projected_size)) {
      REXLOG_TRACE(codegen, "Block 0x{:08X} hit projection limit at size 0x{:X}", block.base,
                   block_size);
      result.blocks.push_back(block);
      block_stack.pop_back();
      continue;
    }

    if (scanned_addrs.count(addr)) {
      if (addr > block.base) {
        block.successors.push_back(addr);
        block.has_terminator = true;
        result.blocks.push_back(block);
      }
      block_stack.pop_back();
      continue;
    }

    if (addr != entry_point && known_callables_.contains(static_cast<u32>(addr))) {
      REXLOG_TRACE(codegen, "discover_blocks: hit entry point 0x{:08X} - stopping block", addr);
      if (addr > block.base) {
        block.end = addr;
        block.has_terminator = true;
        result.blocks.push_back(block);
      }
      block_stack.pop_back();
      continue;
    }

    auto host_ptr = TranslateAddress<u32>(addr);
    if (!host_ptr) {
      REXLOG_DEBUG(codegen, "discover_blocks: invalid address 0x{:08X}", addr);
      block.has_terminator = true;
      result.blocks.push_back(block);
      block_stack.pop_back();
      continue;
    }

    u32 code = memory::load<be_u32>(host_ptr);

    if (code == 0x00000000) {
      block.end = addr;
      block.has_terminator = true;
      result.blocks.push_back(block);
      block_stack.pop_back();
      continue;
    }

    block.end = addr + 4;
    scanned_addrs.insert(addr);

    Instruction instr = DecodeInstruction(addr, code);

    if (instr.IsReturn()) {
      block.has_terminator = true;
      result.blocks.push_back(block);
      block_stack.pop_back();
      continue;
    }

    if (instr.opcode == Opcode::bcctr) {
      auto jt_info = DetectJumpTable(addr);
      if (jt_info.has_value()) {
        result.jump_tables.push_back(jt_info.value());
        for (GuestAddrT target : jt_info->targets) {
          block.successors.push_back(target);
        }
      }
      block.has_terminator = true;
      result.blocks.push_back(block);
      block_stack.pop_back();

      if (jt_info.has_value()) {
        for (GuestAddrT target : jt_info->targets) {
          if (!scanned_addrs.count(target)) {
            DiscoveredBlock jt_block;
            jt_block.base = target;
            jt_block.end = target;
            jt_block.projected_size = -1;
            block_stack.push_back(jt_block);
          }
        }
      }
      continue;
    }

    if (instr.opcode == Opcode::b || instr.opcode == Opcode::ba) {
      if (instr.branch_target.has_value()) {
        GuestAddrT target = instr.branch_target.value();
        block.successors.push_back(target);

        bool is_tail_call = known_callables_.contains(static_cast<u32>(target));

        if (!is_tail_call && target < entry_point) {
          is_tail_call = true;
        }

        // A forward branch over 1MB is treated as a tail call.
        if (!is_tail_call && target > addr && (target - addr) > 0x100000) {
          is_tail_call = true;
        }

        if (!is_tail_call && IsKnownCallable(static_cast<u32>(target))) {
          is_tail_call = true;
        }

        if (!is_tail_call && !IsInternalBranch(static_cast<u32>(addr), static_cast<u32>(target),
                                               static_cast<u32>(entry_point))) {
          is_tail_call = true;
        }

        if (!is_tail_call && IsProloguePattern(target)) {
          REXLOG_TRACE(codegen, "discover_blocks: target 0x{:08X} has prologue pattern (TAIL CALL)",
                       target);
          is_tail_call = true;
        }

        if (is_tail_call) {
          REXLOG_TRACE(codegen, "discover_blocks: b 0x{:08X} -> 0x{:08X} is TAIL CALL", addr,
                       target);
          result.tail_calls.push_back(target);
        } else if (!scanned_addrs.count(target)) {
          REXLOG_TRACE(
              codegen,
              "discover_blocks: b 0x{:08X} -> 0x{:08X} treated as INTERNAL (entry=0x{:08X})", addr,
              target, entry_point);
          bool is_continuous = (target == block.end);
          i64 carry_projection = -1;
          if (is_continuous && block.projected_size != -1) {
            carry_projection = block.projected_size - static_cast<i64>(block.end - block.base);
            if (carry_projection <= 0)
              carry_projection = -1;
          }

          // Pop before push_back, which can reallocate and invalidate block.
          block.has_terminator = true;
          result.blocks.push_back(block);
          block_stack.pop_back();

          DiscoveredBlock target_block;
          target_block.base = target;
          target_block.end = target;
          target_block.projected_size = carry_projection;
          block_stack.push_back(target_block);
          continue;
        }
      }
      block.has_terminator = true;
      result.blocks.push_back(block);
      block_stack.pop_back();
      continue;
    }

    if (instr.IsCall() && instr.branch_target.has_value()) {
      result.external_calls.push_back(instr.branch_target.value());
      continue;
    }

    if ((instr.opcode == Opcode::bclr || instr.opcode == Opcode::bclrl) && !instr.IsReturn()) {
      GuestAddrT fall_through = addr + 4;
      block.successors.push_back(fall_through);
      block.has_terminator = true;
      result.blocks.push_back(block);
      block_stack.pop_back();

      if (!scanned_addrs.count(fall_through)) {
        DiscoveredBlock ft_block;
        ft_block.base = fall_through;
        ft_block.end = fall_through;
        ft_block.projected_size = -1;
        block_stack.push_back(ft_block);
      }
      continue;
    }

    if (instr.IsBranch() && instr.branch_target.has_value()) {
      GuestAddrT target = instr.branch_target.value();
      GuestAddrT fall_through = addr + 4;

      block.successors.push_back(fall_through);
      block.successors.push_back(target);
      result.blocks.push_back(block);
      block_stack.pop_back();

      bool target_is_internal = (target >= entry_point);

      if (target_is_internal && !scanned_addrs.count(target)) {
        DiscoveredBlock true_block;
        true_block.base = target;
        true_block.end = target;
        true_block.projected_size = -1;
        block_stack.push_back(true_block);
      }

      if (!scanned_addrs.count(fall_through)) {
        DiscoveredBlock false_block;
        false_block.base = fall_through;
        false_block.end = fall_through;

        if (target_is_internal && target > fall_through) {
          false_block.projected_size = static_cast<i64>(target - fall_through);
          REXLOG_TRACE(
              codegen,
              "Conditional branch at 0x{:08X}: fall-through 0x{:08X} projected to 0x{:X} bytes",
              addr, fall_through, false_block.projected_size);
        } else {
          false_block.projected_size = -1;
        }
        block_stack.push_back(false_block);
      }
      continue;
    }
  }

  if (result.blocks.empty()) {
    REXLOG_WARN(codegen, "discover_blocks: no blocks found for entry 0x{:08X}", entry_point);
  }

  std::sort(result.blocks.begin(), result.blocks.end(),
            [](const DiscoveredBlock& a, const DiscoveredBlock& b) { return a.base < b.base; });

  return result;
}

const CodeRegion* FunctionScanner::FindRegionContaining(u32 address) const {
  if (!code_regions_)
    return nullptr;

  for (const auto& region : *code_regions_) {
    if (region.Contains(address)) {
      return &region;
    }
  }
  return nullptr;
}

/**
 * Reports whether a branch stays inside its function. A configured chunk of the function is
 * internal even across regions. A target in another code region, or in none, is a tail call.
 *
 * @param current_addr Address of the branch.
 * @param target_addr Branch target.
 * @param function_entry Entry of the function holding the branch.
 * @return True when the branch is internal.
 */
bool FunctionScanner::IsInternalBranch(u32 current_addr, u32 target_addr,
                                       u32 function_entry) const {
  if (IsWithinChunk(target_addr, function_entry)) {
    return true;
  }

  const auto* current_region = FindRegionContaining(current_addr);
  const auto* target_region = FindRegionContaining(target_addr);

  if (current_region != target_region) {
    REXLOG_TRACE(codegen,
                 "isInternalBranch: 0x{:08X} -> 0x{:08X} crosses region boundary (TAIL CALL)",
                 current_addr, target_addr);
    return false;
  }

  return true;
}

namespace {

[[maybe_unused]]
/**
 * Reports whether an instruction opens a prologue, as mflr (mfspr with SPR 8) or a stwu r1 with a
 * negative displacement.
 *
 * @param insn Instruction to test.
 * @return True when it opens a prologue.
 */
bool IsProloguePattern(const DecodedInsn& insn) {
  using namespace rex::codegen::detail;
  switch (insn.opcode) {
    case Opcode::mflr:
    case Opcode::mfspr:
      return true;
    case Opcode::stwu:
      return insn.D.RA == 1 && insn.D.RT == 1 && static_cast<i16>(insn.D.d) < 0;
    default:
      return false;
  }
}

/**
 * Reports whether a block ends at an instruction. Null or all ones padding and returns end a
 * block. An unknown opcode such as a 64 bit rotate does not, and is emitted as a comment. A bcctrl
 * is a call. An unconditional bcctr ends a block and a conditional one falls through when its
 * condition fails.
 *
 * @param insn Instruction to test.
 * @param region Code region holding the instruction.
 * @param known_functions Function entry points.
 * @return True when the block ends.
 */
bool IsBlockTerminator(const DecodedInsn& insn, const CodeRegion& region,
                       const std::unordered_set<u32>& known_functions) {
  using namespace rex::codegen::detail;

  u32 raw = static_cast<u32>(insn.code);
  if (raw == 0x00000000 || raw == 0xFFFFFFFF) {
    return true;
  }

  if (IsReturn(insn))
    return true;

  if (insn.opcode == Opcode::bcctr || insn.opcode == Opcode::bcctrl) {
    return insn.opcode == Opcode::bcctr && !IsConditional(insn);
  }

  if (IsBranch(insn) && !IsConditional(insn) && !IsCall(insn)) {
    auto target = GetBranchTarget(insn);
    if (target) {
      if (!region.Contains(*target))
        return true;
      if (known_functions.contains(*target))
        return true;
    }
    return true;
  }

  return false;
}

/**
 * Scans back from a bctr for the bounds of its index register. The scan starts no lower than the
 * function start and stops at an unconditional terminator, since earlier comparisons are
 * unrelated. A cmpli or cmpi on the register gives its immediate plus 1 entries and a clrlwi
 * (rlwinm with SH 0, ME 31 and MB above 0) gives 2^(32-MB).
 *
 * @param decoded Decoded binary.
 * @param limits Scan limits.
 * @param bctr_addr Address of the bctr.
 * @param region Code region holding the bctr.
 * @param expected_reg Register holding the switch index.
 * @param func_start Function start address.
 * @return The bounds, with found set when one was seen.
 */
BoundsInfo ScanForBounds(DecodedBinary& decoded, const Limits& limits, u32 bctr_addr,
                         const CodeRegion& region, u8 expected_reg, u32 func_start) {
  BoundsInfo result;
  const int backward_scan_limit = static_cast<int>(limits.backward_scan_limit);

  u32 scan_lower_bound = std::max(region.start, func_start);

  REXLOG_TRACE(
      codegen,
      "scanForBounds: bctr=0x{:08X} region=[0x{:08X}-0x{:08X}] funcStart=0x{:08X} expectedReg=r{}",
      bctr_addr, region.start, region.end, func_start, expected_reg);

  u32 scan_addr = bctr_addr;
  for (int i = 0; i < backward_scan_limit && scan_addr >= scan_lower_bound + 4; i++) {
    scan_addr -= 4;
    auto* insn = decoded.FindInsn(scan_addr);
    if (!insn)
      break;

    if (IsTerminator(*insn) && !IsConditional(*insn))
      break;

    using namespace rex::codegen::detail;

    if (insn->opcode == Opcode::cmpli) {
      REXLOG_TRACE(codegen, "scanForBounds: found cmpli at 0x{:08X} RA=r{} UIMM={} (expecting r{})",
                   scan_addr, static_cast<unsigned>(insn->D.RA), static_cast<int>(insn->D.d),
                   expected_reg);
      if (insn->D.RA == expected_reg) {
        result.max_entries = static_cast<u32>(insn->D.d) + 1;
        result.index_reg = expected_reg;
        result.found = true;
        REXLOG_TRACE(codegen, "scanForBounds: MATCHED! maxEntries={}", result.max_entries);
        return result;
      }
    }

    if (insn->opcode == Opcode::cmpi) {
      REXLOG_TRACE(codegen, "scanForBounds: found cmpi at 0x{:08X} RA=r{} SIMM={} (expecting r{})",
                   scan_addr, static_cast<unsigned>(insn->D.RA), static_cast<int>(insn->D.d),
                   expected_reg);
      if (insn->D.RA == expected_reg) {
        result.max_entries = static_cast<u32>(insn->D.d) + 1;
        result.index_reg = expected_reg;
        result.found = true;
        REXLOG_TRACE(codegen, "scanForBounds: MATCHED! maxEntries={}", result.max_entries);
        return result;
      }
    }

    if (insn->opcode == Opcode::rlwinm) {
      if (insn->M.RA == expected_reg && insn->M.SH == 0 && insn->M.ME == 31 && insn->M.MB > 0) {
        u32 bits = 32 - insn->M.MB;
        result.max_entries = 1u << bits;
        result.index_reg = expected_reg;
        result.found = true;
        REXLOG_TRACE(codegen, "scanForBounds: found clrlwi at 0x{:08X} MB={} maxEntries={}",
                     scan_addr, static_cast<unsigned>(insn->M.MB), result.max_entries);
        return result;
      }
    }
  }

  REXLOG_TRACE(codegen, "scanForBounds: no bounds found for bctr=0x{:08X}", bctr_addr);
  return result;
}

}  // anonymous namespace

/**
 * Detects a jump table by scanning back from a bctr through mtctr, the load, the optional add of a
 * base, and the lis and addi or ori pairs of the table and base addresses. The scan does not stop
 * at conditional branches, which are often bounds checks. The index register is traced back
 * through slwi only (rlwinm with SH above 0, MB 0 and ME 31-SH) and tracing stops when another
 * instruction writes it. An addi low half is sign extended and an ori low half is OR ed. Targets
 * must be 4 byte aligned, inside the code region and not below the function start. A target
 * pointing at null padding is stored as the sentinel 0, which codegen emits as a trap.
 *
 * @param decoded Decoded binary.
 * @param limits Scan limits.
 * @param bctr_addr Address of the bctr.
 * @param containing_region Code region holding the bctr.
 * @param func_start Function start address.
 * @param func_end Function end address.
 * @return The table, or nullopt when none matches.
 */
std::optional<JumpTable> DetectJumpTable(DecodedBinary& decoded, const Limits& limits,
                                         u32 bctr_addr, const CodeRegion& containing_region,
                                         u32 func_start, u32 func_end) {
  using namespace rex::codegen::detail;

  const int k_max_backward_scan = static_cast<int>(limits.backward_scan_limit);
  const u32 k_max_table_entries = limits.max_jump_table_entries;

  u8 ctr_source_reg = 0xFF;
  u32 table_addr = 0;
  u32 base_addr = 0;
  u32 pending_table_lo = 0, pending_table_hi = 0;
  u32 pending_base_lo = 0, pending_base_hi = 0;
  bool has_pending_table_lo = false, has_pending_table_hi = false;
  bool has_pending_base_lo = false, has_pending_base_hi = false;
  bool pending_table_lo_is_addi = false, pending_base_lo_is_addi = false;

  auto combine_hi_lo = [](u32 hi, u32 lo, bool is_addi) -> u32 {
    if (is_addi) {
      return hi + static_cast<u32>(static_cast<i32>(static_cast<i16>(lo)));
    } else {
      return hi | lo;
    }
  };
  JumpTableType table_type = JumpTableType::kAbsolute;
  u8 index_reg = 0xFF;
  u8 final_index_reg = 0xFF;
  int shift_amount = 0;

  u32 scan_addr = bctr_addr;
  bool found_mtctr = false;
  bool found_load = false;

  for (int i = 0; i < k_max_backward_scan && scan_addr >= containing_region.start + 4; i++) {
    scan_addr -= 4;
    auto* insn = decoded.FindInsn(scan_addr);
    if (!insn)
      break;

    if (IsTerminator(*insn) && !IsConditional(*insn))
      break;

    if (!found_mtctr) {
      if (insn->opcode == Opcode::mtctr || insn->opcode == Opcode::mtspr) {
        ctr_source_reg = insn->XFX.RT;
        found_mtctr = true;
        continue;
      }
    }

    if (found_mtctr && !found_load) {
      if (insn->opcode == Opcode::lwzx && insn->X.RT == ctr_source_reg) {
        table_type = JumpTableType::kAbsolute;
        index_reg = insn->X.RB;
        final_index_reg = index_reg;
        found_load = true;
        REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} found lwzx at 0x{:08X}", bctr_addr,
                     scan_addr);
        continue;
      }

      if (insn->opcode == Opcode::lbzx && insn->X.RT == ctr_source_reg) {
        if (table_type != JumpTableType::kComputed) {
          table_type = JumpTableType::kByteOffset;
        }
        index_reg = insn->X.RB;
        final_index_reg = index_reg;
        found_load = true;
        REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} found lbzx at 0x{:08X}", bctr_addr,
                     scan_addr);
        continue;
      }

      if (insn->opcode == Opcode::lhzx && insn->X.RT == ctr_source_reg) {
        if (table_type != JumpTableType::kComputed) {
          table_type = JumpTableType::kShortOffset;
        }
        index_reg = insn->X.RB;
        final_index_reg = index_reg;
        found_load = true;
        REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} found lhzx at 0x{:08X}", bctr_addr,
                     scan_addr);
        continue;
      }

      if (insn->opcode == Opcode::add && insn->XO.RT == ctr_source_reg) {
        if (insn->XO.RA == ctr_source_reg) {
          ctr_source_reg = insn->XO.RB;
        } else {
          ctr_source_reg = insn->XO.RA;
        }
        REXLOG_TRACE(codegen,
                     "detectJumpTable: bctr=0x{:08X} found add at 0x{:08X}, now tracking r{}",
                     bctr_addr, scan_addr, ctr_source_reg);
        continue;
      }

      if (insn->opcode == Opcode::rlwinm && insn->M.RA == ctr_source_reg) {
        shift_amount = insn->M.SH;
        if (shift_amount > 0) {
          table_type = JumpTableType::kComputed;
        }
        ctr_source_reg = insn->M.RS;
        REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} found rlwinm at 0x{:08X}", bctr_addr,
                     scan_addr);
        continue;
      }

      REXLOG_TRACE(
          codegen,
          "detectJumpTable: bctr=0x{:08X} unhandled insn at 0x{:08X} opcode={} while looking for "
          "load into r{}",
          bctr_addr, scan_addr, static_cast<int>(insn->opcode), ctr_source_reg);
    }

    if (found_load && index_reg != 0xFF) {
      u8 dest_reg = 0xFF;

      if (insn->opcode == Opcode::rlwinm) {
        dest_reg = insn->M.RA;
      } else if (insn->opcode == Opcode::srawi || insn->opcode == Opcode::sraw ||
                 insn->opcode == Opcode::srw || insn->opcode == Opcode::slw ||
                 insn->opcode == Opcode::or_ || insn->opcode == Opcode::and_ ||
                 insn->opcode == Opcode::xor_ || insn->opcode == Opcode::mr) {
        dest_reg = insn->X.RA;
      } else if (insn->opcode == Opcode::lbz || insn->opcode == Opcode::lhz ||
                 insn->opcode == Opcode::lwz || insn->opcode == Opcode::li ||
                 insn->opcode == Opcode::lis || insn->opcode == Opcode::addi) {
        dest_reg = insn->D.RT;
      } else if (insn->opcode == Opcode::lbzx || insn->opcode == Opcode::lhzx ||
                 insn->opcode == Opcode::lwzx) {
        dest_reg = insn->X.RT;
      } else if (insn->opcode == Opcode::add || insn->opcode == Opcode::subf) {
        dest_reg = insn->XO.RT;
      }

      if (dest_reg == index_reg) {
        if (insn->opcode == Opcode::rlwinm) {
          u8 sh = insn->M.SH;
          u8 mb = insn->M.MB;
          u8 me = insn->M.ME;
          if (sh > 0 && mb == 0 && me == (31 - sh)) {
            index_reg = insn->M.RS;
            final_index_reg = index_reg;
            REXLOG_TRACE(codegen,
                         "detectJumpTable: bctr=0x{:08X} found slwi at 0x{:08X} indexReg now r{}",
                         bctr_addr, scan_addr, index_reg);
          } else {
            REXLOG_TRACE(
                codegen,
                "detectJumpTable: bctr=0x{:08X} indexReg r{} overwritten by non-slwi rlwinm at "
                "0x{:08X}, stop tracing",
                bctr_addr, index_reg, scan_addr);
            index_reg = 0xFF;
          }
        } else {
          REXLOG_TRACE(
              codegen,
              "detectJumpTable: bctr=0x{:08X} indexReg r{} overwritten at 0x{:08X}, stop tracing",
              bctr_addr, index_reg, scan_addr);
          index_reg = 0xFF;
        }
      }
    }

    if (found_mtctr) {
      if (insn->opcode == Opcode::lis) {
        u32 hi = static_cast<u32>(static_cast<u16>(insn->D.d)) << 16;
        REXLOG_TRACE(
            codegen,
            "detectJumpTable: bctr=0x{:08X} found lis at 0x{:08X} hi=0x{:08X} foundLoad={}",
            bctr_addr, scan_addr, hi, found_load);
        if (found_load) {
          if (table_addr == 0) {
            table_addr = has_pending_table_lo
                             ? combine_hi_lo(hi, pending_table_lo, pending_table_lo_is_addi)
                             : hi;
            has_pending_table_lo = false;
            REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} set tableAddr=0x{:08X}",
                         bctr_addr, table_addr);
          }
        } else {
          if (base_addr == 0) {
            base_addr = has_pending_base_lo
                            ? combine_hi_lo(hi, pending_base_lo, pending_base_lo_is_addi)
                            : hi;
            has_pending_base_lo = false;
            REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} set baseAddr=0x{:08X}", bctr_addr,
                         base_addr);
          }
        }
      }

      if (insn->opcode == Opcode::addi || insn->opcode == Opcode::ori) {
        u32 lo = static_cast<u16>(insn->D.d);
        bool is_addi = (insn->opcode == Opcode::addi);
        REXLOG_TRACE(codegen,
                     "detectJumpTable: bctr=0x{:08X} found {} at 0x{:08X} lo=0x{:04X} foundLoad={}",
                     bctr_addr, is_addi ? "addi" : "ori", scan_addr, lo, found_load);
        if (found_load) {
          if (table_addr == 0) {
            if (has_pending_table_hi) {
              table_addr = combine_hi_lo(pending_table_hi, lo, is_addi);
              has_pending_table_hi = false;
              REXLOG_TRACE(codegen,
                           "detectJumpTable: bctr=0x{:08X} set tableAddr=0x{:08X} from pending",
                           bctr_addr, table_addr);
            } else if (!has_pending_table_lo) {
              pending_table_lo = lo;
              pending_table_lo_is_addi = is_addi;
              has_pending_table_lo = true;
              REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} pending tableLo=0x{:04X}",
                           bctr_addr, lo);
            }
          } else if ((table_addr & 0xFFFF) == 0) {
            table_addr = combine_hi_lo(table_addr, lo, is_addi);
            REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} combined tableAddr=0x{:08X}",
                         bctr_addr, table_addr);
          }
        } else {
          if (base_addr == 0) {
            if (has_pending_base_hi) {
              base_addr = combine_hi_lo(pending_base_hi, lo, is_addi);
              has_pending_base_hi = false;
              REXLOG_TRACE(codegen,
                           "detectJumpTable: bctr=0x{:08X} set baseAddr=0x{:08X} from pending",
                           bctr_addr, base_addr);
            } else if (!has_pending_base_lo) {
              pending_base_lo = lo;
              pending_base_lo_is_addi = is_addi;
              has_pending_base_lo = true;
              REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} pending baseLo=0x{:04X}",
                           bctr_addr, lo);
            }
          } else if ((base_addr & 0xFFFF) == 0) {
            base_addr = combine_hi_lo(base_addr, lo, is_addi);
            REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} combined baseAddr=0x{:08X}",
                         bctr_addr, base_addr);
          }
        }
      }
    }
  }

  REXLOG_TRACE(
      codegen,
      "detectJumpTable: bctr=0x{:08X} scan complete: foundMtctr={} foundLoad={} tableAddr=0x{:08X} "
      "baseAddr=0x{:08X}",
      bctr_addr, found_mtctr, found_load, table_addr, base_addr);

  if (!found_mtctr || !found_load || table_addr == 0) {
    REXLOG_TRACE(
        codegen,
        "detectJumpTable: bctr=0x{:08X} FAILED foundMtctr={} foundLoad={} tableAddr=0x{:08X}",
        bctr_addr, found_mtctr, found_load, table_addr);
    return std::nullopt;
  }

  if (table_type != JumpTableType::kAbsolute && base_addr == 0) {
    base_addr = containing_region.start;
  }

  auto bounds =
      ScanForBounds(decoded, limits, bctr_addr, containing_region, final_index_reg, func_start);
  u32 entry_count = bounds.found ? bounds.max_entries : k_max_table_entries;

  JumpTable jt;
  jt.bctr_address = bctr_addr;
  jt.table_address = table_addr;
  jt.index_register = final_index_reg;

  REXLOG_TRACE(
      codegen,
      "detectJumpTable: bctr=0x{:08X} reading {} entries from table=0x{:08X} base=0x{:08X} type={}",
      bctr_addr, entry_count, table_addr, base_addr, static_cast<int>(table_type));

  for (u32 i = 0; i < entry_count; i++) {
    u32 target = 0;

    switch (table_type) {
      case JumpTableType::kAbsolute: {
        auto val = decoded.Read<u32>(table_addr + i * 4);
        if (!val) {
          REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} entry[{}] read failed at 0x{:08X}",
                       bctr_addr, i, table_addr + i * 4);
          break;
        }
        target = *val;
        break;
      }
      case JumpTableType::kByteOffset: {
        auto val = decoded.Read<u8>(table_addr + i);
        if (!val) {
          REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} entry[{}] read failed at 0x{:08X}",
                       bctr_addr, i, table_addr + i);
          break;
        }
        target = base_addr + *val;
        REXLOG_TRACE(codegen,
                     "detectJumpTable: bctr=0x{:08X} entry[{}] offset=0x{:02X} target=0x{:08X}",
                     bctr_addr, i, *val, target);
        break;
      }
      case JumpTableType::kComputed: {
        auto val = decoded.Read<u8>(table_addr + i);
        if (!val)
          break;
        target = base_addr + (static_cast<u32>(*val) << shift_amount);
        break;
      }
      case JumpTableType::kShortOffset: {
        auto val = decoded.Read<u16>(table_addr + i * 2);
        if (!val)
          break;
        target = base_addr + *val;
        break;
      }
    }

    if (target & 3) {
      REXLOG_TRACE(codegen,
                   "detectJumpTable: bctr=0x{:08X} entry[{}] target=0x{:08X} not 4-byte aligned",
                   bctr_addr, i, target);
      if (jt.targets.empty())
        return std::nullopt;
      break;
    }

    if (target == 0 || !containing_region.Contains(target)) {
      if (target != 0) {
        auto outside_insn = decoded.Read<u32>(target);
        if (outside_insn && (*outside_insn == 0x00000000 || *outside_insn == 0xFFFFFFFF)) {
          REXLOG_TRACE(
              codegen,
              "detectJumpTable: bctr=0x{:08X} entry[{}] target=0x{:08X} null jump sentinel",
              bctr_addr, i, target);
          jt.targets.push_back(0);
          continue;
        }
      }
      REXLOG_TRACE(codegen,
                   "detectJumpTable: bctr=0x{:08X} entry[{}] target=0x{:08X} invalid (region "
                   "0x{:08X}-0x{:08X})",
                   bctr_addr, i, target, containing_region.start, containing_region.end);
      if (jt.targets.empty())
        return std::nullopt;
      break;
    }

    if (target < func_start) {
      REXLOG_TRACE(codegen,
                   "detectJumpTable: bctr=0x{:08X} entry[{}] target=0x{:08X} < funcStart=0x{:08X}",
                   bctr_addr, i, target, func_start);
      if (jt.targets.empty())
        return std::nullopt;
      break;
    }

    auto target_insn = decoded.Read<u32>(target);
    if (target_insn && (*target_insn == 0x00000000 || *target_insn == 0xFFFFFFFF)) {
      REXLOG_TRACE(
          codegen,
          "detectJumpTable: bctr=0x{:08X} entry[{}] target=0x{:08X} points to null/padding "
          "(0x{:08X})",
          bctr_addr, i, target, *target_insn);
      jt.targets.push_back(0);
      continue;
    }

    jt.targets.push_back(target);
  }

  if (jt.targets.empty()) {
    REXLOG_TRACE(
        codegen,
        "detectJumpTable: bctr=0x{:08X} table=0x{:08X} NO VALID TARGETS (funcStart=0x{:08X} "
        "funcEnd=0x{:08X})",
        bctr_addr, table_addr, func_start, func_end);
    return std::nullopt;
  }

  REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} table=0x{:08X} entries={} funcEnd=0x{:08X}",
               bctr_addr, table_addr, jt.targets.size(), func_end);
  return jt;
}

/**
 * Discovers the blocks of a function from its entry. A bl is registered as an unresolved branch and
 * an external call when its target is outside the function. A conditional bcctr falls through when
 * its condition fails, so the scan follows the fall-through and its CTR target gets no label. An
 * unconditional bctr uses a configured switch table first, as those are needed when the compiler
 * emits a table without an adjacent bounds check, then automatic detection. A table target that
 * is another known function is dropped, with a warning for a configured table, which points at an
 * entry the configuration also registers. An internal table target past the function end extends it
 * to cover out-of-line case blocks inside the code region. A linear scan stops at the entry of
 * another known function and adds a tail call to it, keyed by that entry address.
 *
 * @param decoded Decoded binary.
 * @param limits Scan limits.
 * @param entry_point Function entry address.
 * @param containing_region Code region holding the function.
 * @param known_functions Function entry points that bound the function.
 * @param pdata_size Function size from PDATA, or 0 when unknown.
 * @param manual_switch_tables Configured switch tables keyed by bctr address, or nullptr.
 * @return The discovered blocks, labels and calls.
 */
BlockDiscoveryResult DiscoverBlocks(
    DecodedBinary& decoded, const Limits& limits, u32 entry_point,
    const CodeRegion& containing_region, const std::unordered_set<u32>& known_functions,
    u32 pdata_size, const std::unordered_map<u32, JumpTable>* manual_switch_tables) {
  BlockDiscoveryResult result;
  std::unordered_set<u32> visited;
  std::unordered_set<u32> block_starts;
  std::queue<u32> worklist;

  u32 func_end = (pdata_size > 0) ? (entry_point + pdata_size) : containing_region.end;
  const u32 declared_end = func_end;

  REXLOG_TRACE(
      codegen,
      "discoverBlocks: entry=0x{:08X} pdataSize={} funcEnd=0x{:08X} region=[0x{:08X}-0x{:08X}]",
      entry_point, pdata_size, func_end, containing_region.start, containing_region.end);

  auto is_within_function = [&](u32 addr) -> bool {
    return addr >= entry_point && addr < func_end;
  };

  worklist.push(entry_point);
  block_starts.insert(entry_point);

  while (!worklist.empty()) {
    u32 block_start = worklist.front();
    worklist.pop();

    if (visited.contains(block_start))
      continue;
    if (!is_within_function(block_start))
      continue;

    u32 addr = block_start;
    Block block;
    block.base = block_start;
    block.size = 0;

    const bool past_declared_end = block_start >= declared_end;

    while (is_within_function(addr) ||
           (past_declared_end && addr == func_end && addr < containing_region.end)) {
      func_end = std::max(func_end, addr + 4);
      auto* insn = decoded.FindInsn(addr);
      if (!insn) {
        REXLOG_TRACE(codegen, "discoverBlocks: 0x{:08X} no instruction at addr, breaking",
                     entry_point);
        break;
      }

      visited.insert(addr);

      result.instructions.push_back(insn);

      if (IsBranch(*insn)) {
        auto target = GetBranchTarget(*insn);

        auto is_internal_target = [&](u32 t) -> bool {
          if (t < entry_point || t >= func_end) {
            return false;
          }
          if (t != entry_point && known_functions.contains(t)) {
            return false;
          }
          return true;
        };

        if (IsCall(*insn)) {
          if (target) {
            result.unresolved_branches.push_back({addr, *target, true, false});

            if (!is_internal_target(*target)) {
              result.external_calls.push_back(*target);
            }
          }
        } else if (IsReturn(*insn)) {
          block.size = addr - block_start + 4;
          break;
        } else if (insn->opcode == rex::codegen::detail::Opcode::bcctr && IsConditional(*insn)) {
          u32 fallthrough = addr + 4;
          if (is_internal_target(fallthrough)) {
            result.labels.insert(fallthrough);
            if (!visited.contains(fallthrough) && !block_starts.contains(fallthrough)) {
              block_starts.insert(fallthrough);
              worklist.push(fallthrough);
            }
          }
        } else if (insn->opcode == rex::codegen::detail::Opcode::bcctr) {
          REXLOG_TRACE(codegen,
                       "discoverBlocks: bctr at 0x{:08X} in func 0x{:08X}, funcEnd=0x{:08X}", addr,
                       entry_point, func_end);
          std::optional<JumpTable> jt;
          bool jt_is_manual = false;
          if (manual_switch_tables) {
            auto manual_it = manual_switch_tables->find(addr);
            if (manual_it != manual_switch_tables->end()) {
              jt = manual_it->second;
              jt_is_manual = true;
              REXLOG_TRACE(
                  codegen,
                  "discoverBlocks: using manual jump table at bctr 0x{:08X} with {} targets", addr,
                  jt->targets.size());
            }
          }
          if (!jt) {
            jt = DetectJumpTable(decoded, limits, addr, containing_region, entry_point, func_end);
          }
          if (jt) {
            REXLOG_TRACE(codegen,
                         "discoverBlocks: detected jump table at bctr 0x{:08X} with {} targets",
                         addr, jt->targets.size());
            result.jump_tables.push_back(*jt);
            for (u32 t : jt->targets) {
              if (t == 0)
                continue;

              if (t != entry_point && known_functions.contains(t)) {
                if (jt_is_manual) {
                  REXLOG_WARN(
                      codegen,
                      "discoverBlocks: manual jump table at bctr 0x{:08X} has target 0x{:08X} "
                      "that is registered as a known function entry; dropping it",
                      addr, t);
                } else {
                  REXLOG_TRACE(
                      codegen,
                      "discoverBlocks: auto-detected jump table at bctr 0x{:08X} has target "
                      "0x{:08X} that is a known function entry; dropping it",
                      addr, t);
                }
                continue;
              }

              if (t >= func_end && t < containing_region.end) {
                func_end = t + 4;
              }
              result.labels.insert(t);
              if (!visited.contains(t) && !block_starts.contains(t)) {
                block_starts.insert(t);
                worklist.push(t);
              }
            }
          }
          block.size = addr - block_start + 4;
          break;
        } else if (IsConditional(*insn)) {
          if (target && is_internal_target(*target)) {
            result.labels.insert(*target);
            if (!visited.contains(*target) && !block_starts.contains(*target)) {
              block_starts.insert(*target);
              worklist.push(*target);
            }
          } else if (target) {
            result.unresolved_branches.push_back({addr, *target, false, true});
          }
          // The fall-through needs a label too.
          u32 fallthrough = addr + 4;
          if (is_internal_target(fallthrough)) {
            result.labels.insert(fallthrough);
            if (!visited.contains(fallthrough) && !block_starts.contains(fallthrough)) {
              block_starts.insert(fallthrough);
              worklist.push(fallthrough);
            }
          }
        } else {
          if (target) {
            if (is_internal_target(*target)) {
              result.labels.insert(*target);
              if (!visited.contains(*target) && !block_starts.contains(*target)) {
                block_starts.insert(*target);
                worklist.push(*target);
              }
            } else {
              result.tail_calls.push_back(*target);
              result.unresolved_branches.push_back({addr, *target, false, false});
            }
          }
          block.size = addr - block_start + 4;
          break;
        }
      }

      if (IsBlockTerminator(*insn, containing_region, known_functions)) {
        REXLOG_TRACE(codegen, "discoverBlocks: 0x{:08X} block terminator at 0x{:08X}", entry_point,
                     addr);
        block.size = addr - block_start + 4;
        break;
      }

      addr += 4;
      if (addr != entry_point && known_functions.contains(addr)) {
        block.size = addr - block_start;
        result.fall_throughs.insert(addr);
        result.tail_calls.push_back(addr);
        result.unresolved_branches.push_back({addr, addr, false, false});
        break;
      }
    }

    if (block.size == 0 && !is_within_function(addr)) {
      REXLOG_TRACE(codegen,
                   "discoverBlocks: 0x{:08X} addr 0x{:08X} outside function (funcEnd=0x{:08X})",
                   entry_point, addr, func_end);
    }

    if (block.size == 0) {
      block.size = addr - block_start;
    }

    if (block.size > 0) {
      result.blocks.push_back(block);
    }
  }

  std::sort(result.blocks.begin(), result.blocks.end(),
            [](const Block& a, const Block& b) { return a.base < b.base; });

  std::sort(result.instructions.begin(), result.instructions.end(),
            [](auto* a, auto* b) { return a->address < b->address; });
  result.instructions.erase(std::unique(result.instructions.begin(), result.instructions.end()),
                            result.instructions.end());

  REXLOG_TRACE(codegen, "discoverBlocks: entry=0x{:08X} blocks={} instructions={} labels={}",
               entry_point, result.blocks.size(), result.instructions.size(), result.labels.size());

  return result;
}

std::optional<u32> FindReturnCut(const BlockDiscoveryResult& result, u32 entry_point) {
  struct Edge {
    u32 site;
    u32 target;
  };
  std::vector<Edge> guards;
  std::vector<Edge> branches;
  for (const Instruction* insn : result.instructions) {
    if (!insn->branch_target || insn->IsCall())
      continue;
    branches.push_back({insn->address, *insn->branch_target});
    if (insn->IsConditional() && result.labels.contains(*insn->branch_target))
      guards.push_back({insn->address, *insn->branch_target});
  }
  for (const JumpTable& table : result.jump_tables) {
    for (u32 target : table.targets) {
      if (target == 0)
        continue;
      branches.push_back({table.bctr_address, target});
      if (result.labels.contains(target))
        guards.push_back({table.bctr_address, target});
    }
  }

  for (const Instruction* insn : result.instructions) {
    const u32 at = insn->address;
    if (!insn->IsReturn() || at == result.instructions.back()->address)
      continue;
    const bool guarded = std::ranges::any_of(
        guards, [&](const Edge& edge) { return edge.site <= at && edge.target > at; });
    const bool closed = std::ranges::none_of(branches, [&](const Edge& edge) {
      return edge.site > at && edge.target > entry_point && edge.target <= at;
    });
    if (!guarded && closed)
      return at + 4;
  }
  return std::nullopt;
}

}  // namespace rex::codegen
