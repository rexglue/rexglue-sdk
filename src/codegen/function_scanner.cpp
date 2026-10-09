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
#include <rex/logging.h>

#include <rex/memory/utils.h>
#include <rex/types.h>

namespace rex::codegen {

using rex::codegen::ppc::decode_instruction;
using rex::codegen::ppc::Instruction;
using rex::codegen::ppc::Opcode;

FunctionScanner::FunctionScanner(const BinaryView& binary, const Limits& limits)
    : binary_(&binary), limits_(&limits) {}

template <typename T>
const T* FunctionScanner::translate_address(rex::guest_addr_t guest_addr) const {
  return reinterpret_cast<const T*>(binary_->translate(static_cast<u32>(guest_addr)));
}

bool FunctionScanner::isExecutableSection(rex::guest_addr_t address) const {
  return binary_->isExecutable(static_cast<u32>(address));
}

bool FunctionScanner::is_prologue_pattern(guest_addr_t address) {
  auto host_ptr = translate_address<u32>(address);
  if (!host_ptr)
    return false;

  u32 code = memory::load<be_u32>(host_ptr);
  Instruction instr = decode_instruction(address, code);

  if (instr.opcode == Opcode::mflr) {
    return true;
  }

  if (instr.opcode == Opcode::mfspr && instr.XFX.spr_num() == 8) {
    return true;
  }

  if (instr.opcode == Opcode::stwu && instr.D.RS() == 1 && instr.D.RA == 1 && instr.D.SIMM() < 0) {
    return true;
  }

  return false;
}

bool FunctionScanner::is_epilogue_pattern(guest_addr_t address) {
  auto host_ptr = translate_address<u32>(address);
  if (!host_ptr)
    return false;

  u32 code = memory::load<be_u32>(host_ptr);

  Instruction instr = decode_instruction(address, code);

  if (instr.is_return()) {
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
bool FunctionScanner::is_restgprlr_function(guest_addr_t address) {
  auto host_ptr = translate_address<u32>(address);
  if (!host_ptr)
    return false;

  u32 first = memory::load<be_u32>(host_ptr);

  constexpr u32 RESTGPRLR_14 = 0xe9c1ff68;
  constexpr u32 SAVEGPRLR_14 = 0xf9c1ff68;
  constexpr u32 RESTFPR_14 = 0xc9ccff70;
  constexpr u32 SAVEFPR_14 = 0xd9ccff70;

  if (first == RESTGPRLR_14 || first == SAVEGPRLR_14 || first == RESTFPR_14 ||
      first == SAVEFPR_14) {
    return true;
  }

  if (first == 0x3960fee0) {
    auto second_ptr = translate_address<u32>(address + 4);
    if (second_ptr) {
      u32 second = memory::load<be_u32>(second_ptr);
      constexpr u32 RESTVMX_14 = 0x7dcb60ce;
      constexpr u32 SAVEVMX_14 = 0x7dcb61ce;
      if (second == RESTVMX_14 || second == SAVEVMX_14) {
        return true;
      }
    }
  }

  if (first == 0x3960fc00) {
    auto second_ptr = translate_address<u32>(address + 4);
    if (second_ptr) {
      u32 second = memory::load<be_u32>(second_ptr);
      constexpr u32 RESTVMX_64 = 0x100b60cb;
      constexpr u32 SAVEVMX_64 = 0x100b61cb;
      if (second == RESTVMX_64 || second == SAVEVMX_64) {
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
  guest_addr_t table_high = 0;
  guest_addr_t table_low = 0;
  guest_addr_t base_high = 0;
  guest_addr_t base_low = 0;

  bool found_mtctr = false;
  bool found_add = false;
  bool found_load = false;
  bool found_shift = false;
  bool found_table_lis = false;
  bool found_table_addi = false;
  bool found_base_lis = false;
  bool found_base_addi = false;

  guest_addr_t table_address() const { return table_high + static_cast<i16>(table_low); }
  guest_addr_t base_address() const { return base_high + static_cast<i16>(base_low); }
};

struct BoundsInfo {
  u32 maxEntries = 0;
  u8 indexReg = 0;
  u32 defaultTarget = 0;
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
BoundsInfo scanForBounds(const FunctionScanner& scanner, guest_addr_t bctr_address,
                         u8 expected_index_reg) {
  BoundsInfo bounds;
  constexpr int MAX_SCAN = 64;

  u8 cr_field = 0xFF;

  for (int i = 1; i <= MAX_SCAN; ++i) {
    guest_addr_t addr = bctr_address - (i * 4);
    if (addr < 4)
      break;

    auto host_ptr = scanner.translate_address<u32>(addr);
    if (!host_ptr)
      break;

    u32 code = memory::load<be_u32>(host_ptr);
    if (code == 0)
      break;

    Instruction instr = decode_instruction(addr, code);

    if (cr_field == 0xFF) {
      bool is_cond_branch = (instr.opcode == Opcode::bc || instr.opcode == Opcode::bca ||
                             instr.opcode == Opcode::bcl || instr.opcode == Opcode::bcla ||
                             instr.opcode == Opcode::bclr || instr.opcode == Opcode::bclrl);

      if (is_cond_branch) {
        u8 bi = instr.B.BI;

        if ((bi & 0x3) == 1) {
          cr_field = (bi >> 2) & 0x7;

          if (instr.branch_target.has_value()) {
            bounds.defaultTarget = instr.branch_target.value();
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
          bounds.maxEntries = implicit_count;
          bounds.indexReg = expected_index_reg;
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
        bounds.maxEntries = cmp_imm + 1;
        bounds.indexReg = cmp_ra;
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
std::vector<guest_addr_t> read_table_entries(const FunctionScanner& scanner,
                                             const JumpTableMatch& match, u32 entry_count) {
  std::vector<guest_addr_t> targets;
  guest_addr_t table_addr = match.table_address();

  for (u32 i = 0; entry_count == 0 || i < entry_count; ++i) {
    guest_addr_t target = 0;

    switch (match.type) {
      case JumpTableType::kAbsolute: {
        auto entry_ptr = scanner.translate_address<u32>(table_addr + (i * 4));
        if (!entry_ptr)
          goto done;
        target = memory::load<be_u32>(entry_ptr);
        break;
      }

      case JumpTableType::kComputed: {
        auto entry_ptr = scanner.translate_address<u8>(table_addr + i);
        if (!entry_ptr)
          goto done;
        u8 offset = *entry_ptr;
        target = match.base_address() + (static_cast<u32>(offset) << match.shift_amount);
        break;
      }

      case JumpTableType::kByteOffset: {
        auto entry_ptr = scanner.translate_address<u8>(table_addr + i);
        if (!entry_ptr)
          goto done;
        u8 offset = *entry_ptr;
        target = match.base_address() + offset;
        break;
      }

      case JumpTableType::kShortOffset: {
        auto entry_ptr = scanner.translate_address<u16>(table_addr + (i * 2));
        if (!entry_ptr)
          goto done;
        u16 offset = memory::load<be_u16>(entry_ptr);
        target = match.base_address() + offset;
        break;
      }
    }

    if (target & 3)
      goto done;

    if (target == 0 && match.type == JumpTableType::kAbsolute)
      goto done;

    if (!scanner.isExecutableSection(target)) {
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
static bool is_function_boundary(u32 code, const Instruction& instr, guest_addr_t addr) {
  if (code == 0x00000000) {
    REXLOG_TRACE(codegen, "  [0x{:08X}] Hit zero padding - function boundary", addr);
    return true;
  }

  if (instr.is_return()) {
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
std::optional<JumpTable> FunctionScanner::detect_jump_table(guest_addr_t bctr_address) {
  if (known_switch_tables_.count(static_cast<u32>(bctr_address))) {
    REXLOG_TRACE(codegen, "detect_jump_table: skipping 0x{:08X} (manual table exists)",
                 bctr_address);
    return std::nullopt;
  }

  const int MAX_SCAN_BACK = static_cast<int>(limits_->backward_scan_limit);

  JumpTableMatch match;

  REXLOG_TRACE(codegen, "detect_jump_table: scanning backward from bctr at 0x{:08X}", bctr_address);

  for (int i = 1; i <= MAX_SCAN_BACK; ++i) {
    guest_addr_t addr = bctr_address - (i * 4);
    if (addr < 4)
      break;

    auto host_ptr = translate_address<u32>(addr);
    if (!host_ptr)
      break;

    u32 code = memory::load<be_u32>(host_ptr);

    Instruction instr = decode_instruction(addr, code);

    if (is_function_boundary(code, instr, addr)) {
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
            match.base_high = static_cast<guest_addr_t>(instr.D.UIMM()) << 16;
            match.found_base_lis = true;
            REXLOG_TRACE(codegen, "  [0x{:08X}] Found base lis r{}, 0x{:04X} (register reuse)",
                         addr, static_cast<u32>(instr.D.RT), instr.D.UIMM());
            continue;
          } else if (!match.found_table_lis) {
            match.table_high = static_cast<guest_addr_t>(instr.D.UIMM()) << 16;
            match.found_table_lis = true;
            REXLOG_TRACE(codegen, "  [0x{:08X}] Found table lis r{}, 0x{:04X} (register reuse)",
                         addr, static_cast<u32>(instr.D.RT), instr.D.UIMM());
            continue;
          }
        } else if (!match.found_table_lis) {
          match.table_high = static_cast<guest_addr_t>(instr.D.UIMM()) << 16;
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
        match.table_high = static_cast<guest_addr_t>(instr.D.UIMM()) << 16;
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
          match.base_high = static_cast<guest_addr_t>(instr.D.UIMM()) << 16;
          match.found_base_lis = true;
          REXLOG_TRACE(codegen, "  [0x{:08X}] Found base lis r{}, 0x{:04X}", addr,
                       static_cast<u32>(instr.D.RT), instr.D.UIMM());
          continue;
        } else if (instr.D.RT == match.offset_reg && !match.found_base_lis) {
          REXLOG_TRACE(codegen, "  [0x{:08X}] Found lis for offset_reg r{}, swapping base/offset",
                       addr, static_cast<u32>(instr.D.RT));
          std::swap(match.base_reg, match.offset_reg);
          match.base_high = static_cast<guest_addr_t>(instr.D.UIMM()) << 16;
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

  guest_addr_t table_address = match.table_address();
  REXLOG_TRACE(codegen, "  Table address: 0x{:08X} (high=0x{:08X}, low=0x{:04X})", table_address,
               match.table_high, match.table_low);

  auto table_ptr = translate_address<u8>(table_address);
  if (!table_ptr) {
    REXLOG_TRACE(codegen, "  Invalid table address 0x{:08X} - not in mapped memory", table_address);
    return std::nullopt;
  }

  BoundsInfo bounds = scanForBounds(*this, bctr_address, match.index_reg);
  REXLOG_TRACE(codegen, "  Bounds check: found={}, count={}, default=0x{:08X}, index_reg=r{}",
               bounds.found, bounds.maxEntries, bounds.defaultTarget, bounds.indexReg);

  std::vector<guest_addr_t> targets = read_table_entries(*this, match, bounds.maxEntries);

  if (targets.size() < 2) {
    REXLOG_TRACE(codegen, "  Insufficient entries: {} (need at least 2)", targets.size());
    return std::nullopt;
  }

  JumpTable jt;
  jt.bctrAddress = static_cast<u32>(bctr_address);
  jt.tableAddress = static_cast<u32>(table_address);
  jt.indexRegister = match.index_reg;
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
FunctionBlocks FunctionScanner::discover_blocks(rex::guest_addr_t entry_point,
                                                rex::u32 pdata_size) {
  FunctionBlocks result;
  result.entry = entry_point;
  result.pdata_size = pdata_size;

  std::unordered_set<guest_addr_t> scannedAddrs;

  std::vector<DiscoveredBlock> block_stack;

  DiscoveredBlock entry_block;
  entry_block.base = entry_point;
  entry_block.end = entry_point;
  entry_block.projectedSize = -1;
  block_stack.push_back(entry_block);

  const size_t MAX_BLOCKS = limits_->max_blocks_per_function;

  while (!block_stack.empty() && result.blocks.size() < MAX_BLOCKS) {
    DiscoveredBlock& block = block_stack.back();

    if (block.end == block.base) {
      if (scannedAddrs.count(block.base)) {
        block_stack.pop_back();
        continue;
      }
    }

    if ((block.base & 0x3) != 0) {
      REXLOG_WARN(codegen, "discover_blocks: misaligned block start 0x{:08X}", block.base);
      block_stack.pop_back();
      continue;
    }

    guest_addr_t addr = block.end;
    if (addr == block.base) {}

    guest_addr_t block_size = addr - block.base;
    if (block.projectedSize != -1 && block_size >= static_cast<guest_addr_t>(block.projectedSize)) {
      REXLOG_TRACE(codegen, "Block 0x{:08X} hit projection limit at size 0x{:X}", block.base,
                   block_size);
      result.blocks.push_back(block);
      block_stack.pop_back();
      continue;
    }

    if (scannedAddrs.count(addr)) {
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

    auto host_ptr = translate_address<u32>(addr);
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
    scannedAddrs.insert(addr);

    Instruction instr = decode_instruction(addr, code);

    if (instr.is_return()) {
      block.has_terminator = true;
      result.blocks.push_back(block);
      block_stack.pop_back();
      continue;
    }

    if (instr.opcode == Opcode::bcctr) {
      auto jt_info = detect_jump_table(addr);
      if (jt_info.has_value()) {
        result.jump_tables.push_back(jt_info.value());
        for (guest_addr_t target : jt_info->targets) {
          block.successors.push_back(target);
        }
      }
      block.has_terminator = true;
      result.blocks.push_back(block);
      block_stack.pop_back();

      if (jt_info.has_value()) {
        for (guest_addr_t target : jt_info->targets) {
          if (!scannedAddrs.count(target)) {
            DiscoveredBlock jt_block;
            jt_block.base = target;
            jt_block.end = target;
            jt_block.projectedSize = -1;
            block_stack.push_back(jt_block);
          }
        }
      }
      continue;
    }

    if (instr.opcode == Opcode::b || instr.opcode == Opcode::ba) {
      if (instr.branch_target.has_value()) {
        guest_addr_t target = instr.branch_target.value();
        block.successors.push_back(target);

        bool is_tail_call = known_callables_.contains(static_cast<u32>(target));

        if (!is_tail_call && target < entry_point) {
          is_tail_call = true;
        }

        // A forward branch over 1MB is treated as a tail call.
        if (!is_tail_call && target > addr && (target - addr) > 0x100000) {
          is_tail_call = true;
        }

        if (!is_tail_call && isKnownCallable(static_cast<u32>(target))) {
          is_tail_call = true;
        }

        if (!is_tail_call && !isInternalBranch(static_cast<u32>(addr), static_cast<u32>(target),
                                               static_cast<u32>(entry_point))) {
          is_tail_call = true;
        }

        if (!is_tail_call && is_prologue_pattern(target)) {
          REXLOG_TRACE(codegen, "discover_blocks: target 0x{:08X} has prologue pattern (TAIL CALL)",
                       target);
          is_tail_call = true;
        }

        if (is_tail_call) {
          REXLOG_TRACE(codegen, "discover_blocks: b 0x{:08X} -> 0x{:08X} is TAIL CALL", addr,
                       target);
          result.tail_calls.push_back(target);
        } else if (!scannedAddrs.count(target)) {
          REXLOG_TRACE(
              codegen,
              "discover_blocks: b 0x{:08X} -> 0x{:08X} treated as INTERNAL (entry=0x{:08X})", addr,
              target, entry_point);
          bool is_continuous = (target == block.end);
          i64 carry_projection = -1;
          if (is_continuous && block.projectedSize != -1) {
            carry_projection = block.projectedSize - static_cast<i64>(block.end - block.base);
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
          target_block.projectedSize = carry_projection;
          block_stack.push_back(target_block);
          continue;
        }
      }
      block.has_terminator = true;
      result.blocks.push_back(block);
      block_stack.pop_back();
      continue;
    }

    if (instr.is_call() && instr.branch_target.has_value()) {
      result.external_calls.push_back(instr.branch_target.value());
      continue;
    }

    if ((instr.opcode == Opcode::bclr || instr.opcode == Opcode::bclrl) && !instr.is_return()) {
      guest_addr_t fall_through = addr + 4;
      block.successors.push_back(fall_through);
      block.has_terminator = true;
      result.blocks.push_back(block);
      block_stack.pop_back();

      if (!scannedAddrs.count(fall_through)) {
        DiscoveredBlock ft_block;
        ft_block.base = fall_through;
        ft_block.end = fall_through;
        ft_block.projectedSize = -1;
        block_stack.push_back(ft_block);
      }
      continue;
    }

    if (instr.is_branch() && instr.branch_target.has_value()) {
      guest_addr_t target = instr.branch_target.value();
      guest_addr_t fall_through = addr + 4;

      block.successors.push_back(fall_through);
      block.successors.push_back(target);
      result.blocks.push_back(block);
      block_stack.pop_back();

      bool target_is_internal = (target >= entry_point);

      if (target_is_internal && !scannedAddrs.count(target)) {
        DiscoveredBlock true_block;
        true_block.base = target;
        true_block.end = target;
        true_block.projectedSize = -1;
        block_stack.push_back(true_block);
      }

      if (!scannedAddrs.count(fall_through)) {
        DiscoveredBlock false_block;
        false_block.base = fall_through;
        false_block.end = fall_through;

        if (target_is_internal && target > fall_through) {
          false_block.projectedSize = static_cast<i64>(target - fall_through);
          REXLOG_TRACE(
              codegen,
              "Conditional branch at 0x{:08X}: fall-through 0x{:08X} projected to 0x{:X} bytes",
              addr, fall_through, false_block.projectedSize);
        } else {
          false_block.projectedSize = -1;
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

const CodeRegion* FunctionScanner::findRegionContaining(u32 address) const {
  if (!code_regions_)
    return nullptr;

  for (const auto& region : *code_regions_) {
    if (region.contains(address)) {
      return &region;
    }
  }
  return nullptr;
}

/**
 * Reports whether a branch stays inside its function. A configured chunk of the function is
 * internal even across regions. A target in another code region, or in none, is a tail call.
 *
 * @param currentAddr Address of the branch.
 * @param targetAddr Branch target.
 * @param functionEntry Entry of the function holding the branch.
 * @return True when the branch is internal.
 */
bool FunctionScanner::isInternalBranch(u32 currentAddr, u32 targetAddr, u32 functionEntry) const {
  if (isWithinChunk(targetAddr, functionEntry)) {
    return true;
  }

  const auto* currentRegion = findRegionContaining(currentAddr);
  const auto* targetRegion = findRegionContaining(targetAddr);

  if (currentRegion != targetRegion) {
    REXLOG_TRACE(codegen,
                 "isInternalBranch: 0x{:08X} -> 0x{:08X} crosses region boundary (TAIL CALL)",
                 currentAddr, targetAddr);
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
bool isProloguePattern(const DecodedInsn& insn) {
  using namespace rex::codegen::ppc;
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
 * @param knownFunctions Function entry points.
 * @return True when the block ends.
 */
bool isBlockTerminator(const DecodedInsn& insn, const CodeRegion& region,
                       const std::unordered_set<u32>& knownFunctions) {
  using namespace rex::codegen::ppc;

  u32 raw = static_cast<u32>(insn.code);
  if (raw == 0x00000000 || raw == 0xFFFFFFFF) {
    return true;
  }

  if (isReturn(insn))
    return true;

  if (insn.opcode == Opcode::bcctr || insn.opcode == Opcode::bcctrl) {
    return insn.opcode == Opcode::bcctr && !isConditional(insn);
  }

  if (isBranch(insn) && !isConditional(insn) && !isCall(insn)) {
    auto target = getBranchTarget(insn);
    if (target) {
      if (!region.contains(*target))
        return true;
      if (knownFunctions.contains(*target))
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
 * @param bctrAddr Address of the bctr.
 * @param region Code region holding the bctr.
 * @param expectedReg Register holding the switch index.
 * @param funcStart Function start address.
 * @return The bounds, with found set when one was seen.
 */
BoundsInfo scanForBounds(DecodedBinary& decoded, const Limits& limits, u32 bctrAddr,
                         const CodeRegion& region, u8 expectedReg, u32 funcStart) {
  BoundsInfo result;
  const int backwardScanLimit = static_cast<int>(limits.backward_scan_limit);

  u32 scanLowerBound = std::max(region.start, funcStart);

  REXLOG_TRACE(
      codegen,
      "scanForBounds: bctr=0x{:08X} region=[0x{:08X}-0x{:08X}] funcStart=0x{:08X} expectedReg=r{}",
      bctrAddr, region.start, region.end, funcStart, expectedReg);

  u32 scanAddr = bctrAddr;
  for (int i = 0; i < backwardScanLimit && scanAddr >= scanLowerBound + 4; i++) {
    scanAddr -= 4;
    auto* insn = decoded.get(scanAddr);
    if (!insn)
      break;

    if (isTerminator(*insn) && !isConditional(*insn))
      break;

    using namespace rex::codegen::ppc;

    if (insn->opcode == Opcode::cmpli) {
      REXLOG_TRACE(codegen, "scanForBounds: found cmpli at 0x{:08X} RA=r{} UIMM={} (expecting r{})",
                   scanAddr, static_cast<unsigned>(insn->D.RA), static_cast<int>(insn->D.d),
                   expectedReg);
      if (insn->D.RA == expectedReg) {
        result.maxEntries = static_cast<u32>(insn->D.d) + 1;
        result.indexReg = expectedReg;
        result.found = true;
        REXLOG_TRACE(codegen, "scanForBounds: MATCHED! maxEntries={}", result.maxEntries);
        return result;
      }
    }

    if (insn->opcode == Opcode::cmpi) {
      REXLOG_TRACE(codegen, "scanForBounds: found cmpi at 0x{:08X} RA=r{} SIMM={} (expecting r{})",
                   scanAddr, static_cast<unsigned>(insn->D.RA), static_cast<int>(insn->D.d),
                   expectedReg);
      if (insn->D.RA == expectedReg) {
        result.maxEntries = static_cast<u32>(insn->D.d) + 1;
        result.indexReg = expectedReg;
        result.found = true;
        REXLOG_TRACE(codegen, "scanForBounds: MATCHED! maxEntries={}", result.maxEntries);
        return result;
      }
    }

    if (insn->opcode == Opcode::rlwinm) {
      if (insn->M.RA == expectedReg && insn->M.SH == 0 && insn->M.ME == 31 && insn->M.MB > 0) {
        u32 bits = 32 - insn->M.MB;
        result.maxEntries = 1u << bits;
        result.indexReg = expectedReg;
        result.found = true;
        REXLOG_TRACE(codegen, "scanForBounds: found clrlwi at 0x{:08X} MB={} maxEntries={}",
                     scanAddr, static_cast<unsigned>(insn->M.MB), result.maxEntries);
        return result;
      }
    }
  }

  REXLOG_TRACE(codegen, "scanForBounds: no bounds found for bctr=0x{:08X}", bctrAddr);
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
 * @param bctrAddr Address of the bctr.
 * @param containingRegion Code region holding the bctr.
 * @param funcStart Function start address.
 * @param funcEnd Function end address.
 * @return The table, or nullopt when none matches.
 */
std::optional<JumpTable> detectJumpTable(DecodedBinary& decoded, const Limits& limits, u32 bctrAddr,
                                         const CodeRegion& containingRegion, u32 funcStart,
                                         u32 funcEnd) {
  using namespace rex::codegen::ppc;

  const int kMaxBackwardScan = static_cast<int>(limits.backward_scan_limit);
  const u32 kMaxTableEntries = limits.max_jump_table_entries;

  u8 ctrSourceReg = 0xFF;
  u32 tableAddr = 0;
  u32 baseAddr = 0;
  u32 pendingTableLo = 0, pendingTableHi = 0;
  u32 pendingBaseLo = 0, pendingBaseHi = 0;
  bool hasPendingTableLo = false, hasPendingTableHi = false;
  bool hasPendingBaseLo = false, hasPendingBaseHi = false;
  bool pendingTableLoIsAddi = false, pendingBaseLoIsAddi = false;

  auto combineHiLo = [](u32 hi, u32 lo, bool isAddi) -> u32 {
    if (isAddi) {
      return hi + static_cast<u32>(static_cast<i32>(static_cast<i16>(lo)));
    } else {
      return hi | lo;
    }
  };
  JumpTableType tableType = JumpTableType::kAbsolute;
  u8 indexReg = 0xFF;
  u8 finalIndexReg = 0xFF;
  int shiftAmount = 0;

  u32 scanAddr = bctrAddr;
  bool foundMtctr = false;
  bool foundLoad = false;

  for (int i = 0; i < kMaxBackwardScan && scanAddr >= containingRegion.start + 4; i++) {
    scanAddr -= 4;
    auto* insn = decoded.get(scanAddr);
    if (!insn)
      break;

    if (isTerminator(*insn) && !isConditional(*insn))
      break;

    if (!foundMtctr) {
      if (insn->opcode == Opcode::mtctr || insn->opcode == Opcode::mtspr) {
        ctrSourceReg = insn->XFX.RT;
        foundMtctr = true;
        continue;
      }
    }

    if (foundMtctr && !foundLoad) {
      if (insn->opcode == Opcode::lwzx && insn->X.RT == ctrSourceReg) {
        tableType = JumpTableType::kAbsolute;
        indexReg = insn->X.RB;
        finalIndexReg = indexReg;
        foundLoad = true;
        REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} found lwzx at 0x{:08X}", bctrAddr,
                     scanAddr);
        continue;
      }

      if (insn->opcode == Opcode::lbzx && insn->X.RT == ctrSourceReg) {
        if (tableType != JumpTableType::kComputed) {
          tableType = JumpTableType::kByteOffset;
        }
        indexReg = insn->X.RB;
        finalIndexReg = indexReg;
        foundLoad = true;
        REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} found lbzx at 0x{:08X}", bctrAddr,
                     scanAddr);
        continue;
      }

      if (insn->opcode == Opcode::lhzx && insn->X.RT == ctrSourceReg) {
        if (tableType != JumpTableType::kComputed) {
          tableType = JumpTableType::kShortOffset;
        }
        indexReg = insn->X.RB;
        finalIndexReg = indexReg;
        foundLoad = true;
        REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} found lhzx at 0x{:08X}", bctrAddr,
                     scanAddr);
        continue;
      }

      if (insn->opcode == Opcode::add && insn->XO.RT == ctrSourceReg) {
        if (insn->XO.RA == ctrSourceReg) {
          ctrSourceReg = insn->XO.RB;
        } else {
          ctrSourceReg = insn->XO.RA;
        }
        REXLOG_TRACE(codegen,
                     "detectJumpTable: bctr=0x{:08X} found add at 0x{:08X}, now tracking r{}",
                     bctrAddr, scanAddr, ctrSourceReg);
        continue;
      }

      if (insn->opcode == Opcode::rlwinm && insn->M.RA == ctrSourceReg) {
        shiftAmount = insn->M.SH;
        if (shiftAmount > 0) {
          tableType = JumpTableType::kComputed;
        }
        ctrSourceReg = insn->M.RS;
        REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} found rlwinm at 0x{:08X}", bctrAddr,
                     scanAddr);
        continue;
      }

      REXLOG_TRACE(
          codegen,
          "detectJumpTable: bctr=0x{:08X} unhandled insn at 0x{:08X} opcode={} while looking for "
          "load into r{}",
          bctrAddr, scanAddr, static_cast<int>(insn->opcode), ctrSourceReg);
    }

    if (foundLoad && indexReg != 0xFF) {
      u8 destReg = 0xFF;

      if (insn->opcode == Opcode::rlwinm) {
        destReg = insn->M.RA;
      } else if (insn->opcode == Opcode::srawi || insn->opcode == Opcode::sraw ||
                 insn->opcode == Opcode::srw || insn->opcode == Opcode::slw) {
        destReg = insn->X.RA;
      } else if (insn->opcode == Opcode::lbz || insn->opcode == Opcode::lhz ||
                 insn->opcode == Opcode::lwz || insn->opcode == Opcode::li ||
                 insn->opcode == Opcode::lis || insn->opcode == Opcode::addi) {
        destReg = insn->D.RT;
      } else if (insn->opcode == Opcode::lbzx || insn->opcode == Opcode::lhzx ||
                 insn->opcode == Opcode::lwzx) {
        destReg = insn->X.RT;
      } else if (insn->opcode == Opcode::or_ || insn->opcode == Opcode::and_ ||
                 insn->opcode == Opcode::xor_ || insn->opcode == Opcode::mr) {
        destReg = insn->X.RA;
      } else if (insn->opcode == Opcode::add || insn->opcode == Opcode::subf) {
        destReg = insn->XO.RT;
      }

      if (destReg == indexReg) {
        if (insn->opcode == Opcode::rlwinm) {
          u8 sh = insn->M.SH;
          u8 mb = insn->M.MB;
          u8 me = insn->M.ME;
          if (sh > 0 && mb == 0 && me == (31 - sh)) {
            indexReg = insn->M.RS;
            finalIndexReg = indexReg;
            REXLOG_TRACE(codegen,
                         "detectJumpTable: bctr=0x{:08X} found slwi at 0x{:08X} indexReg now r{}",
                         bctrAddr, scanAddr, indexReg);
          } else {
            REXLOG_TRACE(
                codegen,
                "detectJumpTable: bctr=0x{:08X} indexReg r{} overwritten by non-slwi rlwinm at "
                "0x{:08X}, stop tracing",
                bctrAddr, indexReg, scanAddr);
            indexReg = 0xFF;
          }
        } else {
          REXLOG_TRACE(
              codegen,
              "detectJumpTable: bctr=0x{:08X} indexReg r{} overwritten at 0x{:08X}, stop tracing",
              bctrAddr, indexReg, scanAddr);
          indexReg = 0xFF;
        }
      }
    }

    if (foundMtctr) {
      if (insn->opcode == Opcode::lis) {
        u32 hi = static_cast<u32>(static_cast<u16>(insn->D.d)) << 16;
        REXLOG_TRACE(
            codegen,
            "detectJumpTable: bctr=0x{:08X} found lis at 0x{:08X} hi=0x{:08X} foundLoad={}",
            bctrAddr, scanAddr, hi, foundLoad);
        if (foundLoad) {
          if (tableAddr == 0) {
            tableAddr =
                hasPendingTableLo ? combineHiLo(hi, pendingTableLo, pendingTableLoIsAddi) : hi;
            hasPendingTableLo = false;
            REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} set tableAddr=0x{:08X}", bctrAddr,
                         tableAddr);
          }
        } else {
          if (baseAddr == 0) {
            baseAddr = hasPendingBaseLo ? combineHiLo(hi, pendingBaseLo, pendingBaseLoIsAddi) : hi;
            hasPendingBaseLo = false;
            REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} set baseAddr=0x{:08X}", bctrAddr,
                         baseAddr);
          }
        }
      }

      if (insn->opcode == Opcode::addi || insn->opcode == Opcode::ori) {
        u32 lo = static_cast<u16>(insn->D.d);
        bool isAddi = (insn->opcode == Opcode::addi);
        REXLOG_TRACE(codegen,
                     "detectJumpTable: bctr=0x{:08X} found {} at 0x{:08X} lo=0x{:04X} foundLoad={}",
                     bctrAddr, isAddi ? "addi" : "ori", scanAddr, lo, foundLoad);
        if (foundLoad) {
          if (tableAddr == 0) {
            if (hasPendingTableHi) {
              tableAddr = combineHiLo(pendingTableHi, lo, isAddi);
              hasPendingTableHi = false;
              REXLOG_TRACE(codegen,
                           "detectJumpTable: bctr=0x{:08X} set tableAddr=0x{:08X} from pending",
                           bctrAddr, tableAddr);
            } else if (!hasPendingTableLo) {
              pendingTableLo = lo;
              pendingTableLoIsAddi = isAddi;
              hasPendingTableLo = true;
              REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} pending tableLo=0x{:04X}",
                           bctrAddr, lo);
            }
          } else if ((tableAddr & 0xFFFF) == 0) {
            tableAddr = combineHiLo(tableAddr, lo, isAddi);
            REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} combined tableAddr=0x{:08X}",
                         bctrAddr, tableAddr);
          }
        } else {
          if (baseAddr == 0) {
            if (hasPendingBaseHi) {
              baseAddr = combineHiLo(pendingBaseHi, lo, isAddi);
              hasPendingBaseHi = false;
              REXLOG_TRACE(codegen,
                           "detectJumpTable: bctr=0x{:08X} set baseAddr=0x{:08X} from pending",
                           bctrAddr, baseAddr);
            } else if (!hasPendingBaseLo) {
              pendingBaseLo = lo;
              pendingBaseLoIsAddi = isAddi;
              hasPendingBaseLo = true;
              REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} pending baseLo=0x{:04X}",
                           bctrAddr, lo);
            }
          } else if ((baseAddr & 0xFFFF) == 0) {
            baseAddr = combineHiLo(baseAddr, lo, isAddi);
            REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} combined baseAddr=0x{:08X}",
                         bctrAddr, baseAddr);
          }
        }
      }
    }
  }

  REXLOG_TRACE(
      codegen,
      "detectJumpTable: bctr=0x{:08X} scan complete: foundMtctr={} foundLoad={} tableAddr=0x{:08X} "
      "baseAddr=0x{:08X}",
      bctrAddr, foundMtctr, foundLoad, tableAddr, baseAddr);

  if (!foundMtctr || !foundLoad || tableAddr == 0) {
    REXLOG_TRACE(
        codegen,
        "detectJumpTable: bctr=0x{:08X} FAILED foundMtctr={} foundLoad={} tableAddr=0x{:08X}",
        bctrAddr, foundMtctr, foundLoad, tableAddr);
    return std::nullopt;
  }

  if (tableType != JumpTableType::kAbsolute && baseAddr == 0) {
    baseAddr = containingRegion.start;
  }

  auto bounds =
      scanForBounds(decoded, limits, bctrAddr, containingRegion, finalIndexReg, funcStart);
  u32 entryCount = bounds.found ? bounds.maxEntries : kMaxTableEntries;

  JumpTable jt;
  jt.bctrAddress = bctrAddr;
  jt.tableAddress = tableAddr;
  jt.indexRegister = finalIndexReg;

  REXLOG_TRACE(
      codegen,
      "detectJumpTable: bctr=0x{:08X} reading {} entries from table=0x{:08X} base=0x{:08X} type={}",
      bctrAddr, entryCount, tableAddr, baseAddr, static_cast<int>(tableType));

  for (u32 i = 0; i < entryCount; i++) {
    u32 target = 0;

    switch (tableType) {
      case JumpTableType::kAbsolute: {
        auto val = decoded.read<u32>(tableAddr + i * 4);
        if (!val) {
          REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} entry[{}] read failed at 0x{:08X}",
                       bctrAddr, i, tableAddr + i * 4);
          break;
        }
        target = *val;
        break;
      }
      case JumpTableType::kByteOffset: {
        auto val = decoded.read<u8>(tableAddr + i);
        if (!val) {
          REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} entry[{}] read failed at 0x{:08X}",
                       bctrAddr, i, tableAddr + i);
          break;
        }
        target = baseAddr + *val;
        REXLOG_TRACE(codegen,
                     "detectJumpTable: bctr=0x{:08X} entry[{}] offset=0x{:02X} target=0x{:08X}",
                     bctrAddr, i, *val, target);
        break;
      }
      case JumpTableType::kComputed: {
        auto val = decoded.read<u8>(tableAddr + i);
        if (!val)
          break;
        target = baseAddr + (static_cast<u32>(*val) << shiftAmount);
        break;
      }
      case JumpTableType::kShortOffset: {
        auto val = decoded.read<u16>(tableAddr + i * 2);
        if (!val)
          break;
        target = baseAddr + *val;
        break;
      }
    }

    if (target & 3) {
      REXLOG_TRACE(codegen,
                   "detectJumpTable: bctr=0x{:08X} entry[{}] target=0x{:08X} not 4-byte aligned",
                   bctrAddr, i, target);
      if (jt.targets.empty())
        return std::nullopt;
      break;
    }

    if (target == 0 || !containingRegion.contains(target)) {
      if (target != 0) {
        auto outsideInsn = decoded.read<u32>(target);
        if (outsideInsn && (*outsideInsn == 0x00000000 || *outsideInsn == 0xFFFFFFFF)) {
          REXLOG_TRACE(
              codegen,
              "detectJumpTable: bctr=0x{:08X} entry[{}] target=0x{:08X} null jump sentinel",
              bctrAddr, i, target);
          jt.targets.push_back(0);
          continue;
        }
      }
      REXLOG_TRACE(codegen,
                   "detectJumpTable: bctr=0x{:08X} entry[{}] target=0x{:08X} invalid (region "
                   "0x{:08X}-0x{:08X})",
                   bctrAddr, i, target, containingRegion.start, containingRegion.end);
      if (jt.targets.empty())
        return std::nullopt;
      break;
    }

    if (target < funcStart) {
      REXLOG_TRACE(codegen,
                   "detectJumpTable: bctr=0x{:08X} entry[{}] target=0x{:08X} < funcStart=0x{:08X}",
                   bctrAddr, i, target, funcStart);
      if (jt.targets.empty())
        return std::nullopt;
      break;
    }

    auto targetInsn = decoded.read<u32>(target);
    if (targetInsn && (*targetInsn == 0x00000000 || *targetInsn == 0xFFFFFFFF)) {
      REXLOG_TRACE(
          codegen,
          "detectJumpTable: bctr=0x{:08X} entry[{}] target=0x{:08X} points to null/padding "
          "(0x{:08X})",
          bctrAddr, i, target, *targetInsn);
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
        bctrAddr, tableAddr, funcStart, funcEnd);
    return std::nullopt;
  }

  REXLOG_TRACE(codegen, "detectJumpTable: bctr=0x{:08X} table=0x{:08X} entries={} funcEnd=0x{:08X}",
               bctrAddr, tableAddr, jt.targets.size(), funcEnd);
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
 * to cover out-of-line case blocks inside the code region.
 *
 * @param decoded Decoded binary.
 * @param limits Scan limits.
 * @param entryPoint Function entry address.
 * @param containingRegion Code region holding the function.
 * @param knownFunctions Function entry points that bound the function.
 * @param pdataSize Function size from PDATA, or 0 when unknown.
 * @param manualSwitchTables Configured switch tables keyed by bctr address, or nullptr.
 * @return The discovered blocks, labels and calls.
 */
BlockDiscoveryResult discoverBlocks(DecodedBinary& decoded, const Limits& limits, u32 entryPoint,
                                    const CodeRegion& containingRegion,
                                    const std::unordered_set<u32>& knownFunctions, u32 pdataSize,
                                    const std::unordered_map<u32, JumpTable>* manualSwitchTables) {
  BlockDiscoveryResult result;
  std::unordered_set<u32> visited;
  std::unordered_set<u32> blockStarts;
  std::queue<u32> worklist;

  u32 funcEnd = (pdataSize > 0) ? (entryPoint + pdataSize) : containingRegion.end;

  REXLOG_TRACE(
      codegen,
      "discoverBlocks: entry=0x{:08X} pdataSize={} funcEnd=0x{:08X} region=[0x{:08X}-0x{:08X}]",
      entryPoint, pdataSize, funcEnd, containingRegion.start, containingRegion.end);

  auto isWithinFunction = [&](u32 addr) -> bool { return addr >= entryPoint && addr < funcEnd; };

  worklist.push(entryPoint);
  blockStarts.insert(entryPoint);

  while (!worklist.empty()) {
    u32 blockStart = worklist.front();
    worklist.pop();

    if (visited.contains(blockStart))
      continue;
    if (!isWithinFunction(blockStart))
      continue;

    u32 addr = blockStart;
    Block block;
    block.base = blockStart;
    block.size = 0;

    while (isWithinFunction(addr)) {
      auto* insn = decoded.get(addr);
      if (!insn) {
        REXLOG_TRACE(codegen, "discoverBlocks: 0x{:08X} no instruction at addr, breaking",
                     entryPoint);
        break;
      }

      visited.insert(addr);

      result.instructions.push_back(insn);

      if (isBranch(*insn)) {
        auto target = getBranchTarget(*insn);

        auto isInternalTarget = [&](u32 t) -> bool {
          if (t < entryPoint || t >= funcEnd) {
            return false;
          }
          if (t != entryPoint && knownFunctions.contains(t)) {
            return false;
          }
          return true;
        };

        if (isCall(*insn)) {
          if (target) {
            result.unresolvedBranches.push_back({addr, *target, true, false});

            if (!isInternalTarget(*target)) {
              result.externalCalls.push_back(*target);
            }
          }
        } else if (isReturn(*insn)) {
          block.size = addr - blockStart + 4;
          break;
        } else if (insn->opcode == rex::codegen::ppc::Opcode::bcctr && isConditional(*insn)) {
          u32 fallthrough = addr + 4;
          if (isInternalTarget(fallthrough)) {
            result.labels.insert(fallthrough);
            if (!visited.contains(fallthrough) && !blockStarts.contains(fallthrough)) {
              blockStarts.insert(fallthrough);
              worklist.push(fallthrough);
            }
          }
        } else if (insn->opcode == rex::codegen::ppc::Opcode::bcctr) {
          REXLOG_TRACE(codegen,
                       "discoverBlocks: bctr at 0x{:08X} in func 0x{:08X}, funcEnd=0x{:08X}", addr,
                       entryPoint, funcEnd);
          std::optional<JumpTable> jt;
          bool jtIsManual = false;
          if (manualSwitchTables) {
            auto manualIt = manualSwitchTables->find(addr);
            if (manualIt != manualSwitchTables->end()) {
              jt = manualIt->second;
              jtIsManual = true;
              REXLOG_TRACE(
                  codegen,
                  "discoverBlocks: using manual jump table at bctr 0x{:08X} with {} targets", addr,
                  jt->targets.size());
            }
          }
          if (!jt) {
            jt = detectJumpTable(decoded, limits, addr, containingRegion, entryPoint, funcEnd);
          }
          if (jt) {
            REXLOG_TRACE(codegen,
                         "discoverBlocks: detected jump table at bctr 0x{:08X} with {} targets",
                         addr, jt->targets.size());
            result.jumpTables.push_back(*jt);
            for (u32 t : jt->targets) {
              if (t == 0)
                continue;

              if (t != entryPoint && knownFunctions.contains(t)) {
                if (jtIsManual) {
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

              if (t >= funcEnd && t < containingRegion.end) {
                funcEnd = t + 4;
              }
              result.labels.insert(t);
              if (!visited.contains(t) && !blockStarts.contains(t)) {
                blockStarts.insert(t);
                worklist.push(t);
              }
            }
          }
          block.size = addr - blockStart + 4;
          break;
        } else if (isConditional(*insn)) {
          if (target && isInternalTarget(*target)) {
            result.labels.insert(*target);
            if (!visited.contains(*target) && !blockStarts.contains(*target)) {
              blockStarts.insert(*target);
              worklist.push(*target);
            }
          } else if (target) {
            result.unresolvedBranches.push_back({addr, *target, false, true});
          }
          // The fall-through needs a label too.
          u32 fallthrough = addr + 4;
          if (isInternalTarget(fallthrough)) {
            result.labels.insert(fallthrough);
            if (!visited.contains(fallthrough) && !blockStarts.contains(fallthrough)) {
              blockStarts.insert(fallthrough);
              worklist.push(fallthrough);
            }
          }
        } else {
          if (target) {
            if (isInternalTarget(*target)) {
              result.labels.insert(*target);
              if (!visited.contains(*target) && !blockStarts.contains(*target)) {
                blockStarts.insert(*target);
                worklist.push(*target);
              }
            } else {
              result.tailCalls.push_back(*target);
              result.unresolvedBranches.push_back({addr, *target, false, false});
            }
          }
          block.size = addr - blockStart + 4;
          break;
        }
      }

      if (isBlockTerminator(*insn, containingRegion, knownFunctions)) {
        REXLOG_TRACE(codegen, "discoverBlocks: 0x{:08X} block terminator at 0x{:08X}", entryPoint,
                     addr);
        block.size = addr - blockStart + 4;
        break;
      }

      addr += 4;
    }

    if (block.size == 0 && !isWithinFunction(addr)) {
      REXLOG_TRACE(codegen,
                   "discoverBlocks: 0x{:08X} addr 0x{:08X} outside function (funcEnd=0x{:08X})",
                   entryPoint, addr, funcEnd);
    }

    if (block.size == 0) {
      block.size = addr - blockStart;
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
               entryPoint, result.blocks.size(), result.instructions.size(), result.labels.size());

  return result;
}

}  // namespace rex::codegen
