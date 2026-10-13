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

}  // namespace

namespace {

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
