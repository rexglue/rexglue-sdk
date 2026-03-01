/**
 * @file        discovery.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "codegen_flags.h"
#include "decoded_binary.h"
#include "discovery.h"

#include <algorithm>
#include <queue>

#include <rex/logging.h>
#include <rex/types.h>

namespace rex::codegen {

namespace {

//=============================================================================
// Jump Table Types
//=============================================================================

enum class JumpTableType {
  kAbsolute,    // lwzx - full 32-bit addresses
  kComputed,    // lbzx + rlwinm - byte offset with shift
  kByteOffset,  // lbzx + add - byte offset direct
  kShortOffset  // lhzx + add - 16-bit offset
};

//=============================================================================
// Helper: Check if instruction is prologue pattern
//=============================================================================

[[maybe_unused]]
bool isProloguePattern(const DecodedInsn& insn) {
  using namespace rex::codegen::ppc;
  switch (insn.opcode) {
    case Opcode::mflr:
    case Opcode::mfspr:  // mflr is really mfspr with SPR=8
      return true;
    case Opcode::stwu:
      // stwu r1, -X(r1) - stack frame setup
      return insn.D.RA == 1 && insn.D.RT == 1 && static_cast<int16_t>(insn.D.d) < 0;
    default:
      return false;
  }
}

//=============================================================================
// Helper: Check if block should stop at this instruction
//=============================================================================

bool isBlockTerminator(const DecodedInsn& insn, uint32_t addr, const CodeRegion& region,
                       const std::unordered_set<uint32_t>& knownFunctions) {
  using namespace rex::codegen::ppc;

  // NULL padding ends block (but NOT unknown instructions - those get emitted as comments)
  uint32_t raw = static_cast<uint32_t>(insn.code);
  if (raw == 0x00000000 || raw == 0xFFFFFFFF) {
    return true;
  }
  // Note: kUnknown opcodes (like 64-bit rotate instructions) are NOT terminators.
  // They should be included in the block and emitted as comments during codegen.

  // Check for terminators
  if (isReturn(insn))
    return true;

  // bcctr (indirect branch via CTR)
  if (insn.opcode == Opcode::bcctr || insn.opcode == Opcode::bcctrl) {
    // bcctrl is call, bcctr is terminator
    return insn.opcode == Opcode::bcctr;
  }

  // Unconditional branch
  if (isBranch(insn) && !isConditional(insn) && !isCall(insn)) {
    auto target = getBranchTarget(insn);
    if (target) {
      // Branch outside region is terminator
      if (!region.contains(*target))
        return true;
      // Branch to known function is tail call (terminator)
      if (knownFunctions.contains(*target))
        return true;
    }
    return true;  // Unconditional branch always terminates block
  }

  return false;
}

//=============================================================================
// Helper: Detect bounds check for jump table
//=============================================================================

struct BoundsInfo {
  uint32_t maxEntries = 0;
  uint8_t indexReg = 0;
  bool found = false;
};

BoundsInfo scanForBounds(DecodedBinary& decoded, uint32_t bctrAddr, const CodeRegion& region,
                         uint8_t expectedReg, uint32_t funcStart) {
  BoundsInfo result;
  const int backwardScanLimit = static_cast<int>(REXCVAR_GET(backward_scan_limit));

  // Use funcStart as lower bound to avoid scanning into other functions
  uint32_t scanLowerBound = std::max(region.start, funcStart);

  REXCODEGEN_TRACE(
      "scanForBounds: bctr=0x{:08X} region=[0x{:08X}-0x{:08X}] funcStart=0x{:08X} expectedReg=r{}",
      bctrAddr, region.start, region.end, funcStart, expectedReg);

  uint32_t scanAddr = bctrAddr;
  for (int i = 0; i < backwardScanLimit && scanAddr >= scanLowerBound + 4; i++) {
    scanAddr -= 4;
    auto* insn = decoded.get(scanAddr);
    if (!insn)
      break;

    using namespace rex::codegen::ppc;

    // Look for cmpli/cmpi followed by conditional branch
    if (insn->opcode == Opcode::cmpli) {
      // cmpli crX, L, rA, UIMM
      REXCODEGEN_TRACE("scanForBounds: found cmpli at 0x{:08X} RA=r{} UIMM={} (expecting r{})",
                       scanAddr, static_cast<unsigned>(insn->D.RA), static_cast<int>(insn->D.d),
                       expectedReg);
      if (insn->D.RA == expectedReg) {
        result.maxEntries = static_cast<uint32_t>(insn->D.d) + 1;
        result.indexReg = expectedReg;
        result.found = true;
        REXCODEGEN_TRACE("scanForBounds: MATCHED! maxEntries={}", result.maxEntries);
        return result;
      }
    }

    if (insn->opcode == Opcode::cmpi) {
      // cmpi crX, L, rA, SIMM
      REXCODEGEN_TRACE("scanForBounds: found cmpi at 0x{:08X} RA=r{} SIMM={} (expecting r{})",
                       scanAddr, static_cast<unsigned>(insn->D.RA), static_cast<int>(insn->D.d),
                       expectedReg);
      if (insn->D.RA == expectedReg) {
        result.maxEntries = static_cast<uint32_t>(insn->D.d) + 1;
        result.indexReg = expectedReg;
        result.found = true;
        REXCODEGEN_TRACE("scanForBounds: MATCHED! maxEntries={}", result.maxEntries);
        return result;
      }
    }

    // Look for clrlwi (rlwinm rA, rS, 0, MB, 31) which masks bits
    // MB must be > 0 to actually mask something; MB=0 is a no-op
    if (insn->opcode == Opcode::rlwinm) {
      if (insn->M.RA == expectedReg && insn->M.SH == 0 && insn->M.ME == 31 && insn->M.MB > 0) {
        // Masked to (32 - MB) bits, max value is 2^(32-MB) - 1
        uint32_t bits = 32 - insn->M.MB;
        result.maxEntries = 1u << bits;
        result.indexReg = expectedReg;
        result.found = true;
        REXCODEGEN_TRACE("scanForBounds: found clrlwi at 0x{:08X} MB={} maxEntries={}", scanAddr,
                         static_cast<unsigned>(insn->M.MB), result.maxEntries);
        return result;
      }
    }
  }

  REXCODEGEN_TRACE("scanForBounds: no bounds found for bctr=0x{:08X}", bctrAddr);
  return result;
}

}  // anonymous namespace

//=============================================================================
// Jump Table Detection
//=============================================================================
std::optional<JumpTable> detectJumpTable(DecodedBinary& decoded, uint32_t bctrAddr,
                                         const CodeRegion& containingRegion, uint32_t funcStart,
                                         uint32_t funcEnd) {
  using namespace rex::codegen::ppc;

  const int kMaxBackwardScan = static_cast<int>(REXCVAR_GET(backward_scan_limit));
  const uint32_t kMaxTableEntries = REXCVAR_GET(max_jump_table_entries);

  // State for backward scan
  uint8_t ctrSourceReg = 0xFF;
  uint32_t tableAddr = 0;
  uint32_t baseAddr = 0;
  // Pending address parts (order of lis/addi varies in backward scan)
  // Note: addi uses sign-extended addition, ori uses OR
  uint32_t pendingTableLo = 0, pendingTableHi = 0;
  uint32_t pendingBaseLo = 0, pendingBaseHi = 0;
  bool hasPendingTableLo = false, hasPendingTableHi = false;
  bool hasPendingBaseLo = false, hasPendingBaseHi = false;
  bool pendingTableLoIsAddi = false, pendingBaseLoIsAddi = false;

  // Helper to combine lis high bits with addi/ori low bits
  auto combineHiLo = [](uint32_t hi, uint32_t lo, bool isAddi) -> uint32_t {
    if (isAddi) {
      // addi: sign-extend lo and add
      return hi + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(lo)));
    } else {
      // ori: zero-extend and OR
      return hi | lo;
    }
  };
  JumpTableType tableType = JumpTableType::kAbsolute;
  uint8_t indexReg = 0xFF;       // Current reg being traced (0xFF = stop tracing)
  uint8_t finalIndexReg = 0xFF;  // Last valid indexReg for scanForBounds/output
  int shiftAmount = 0;

  // Backward scan from bctr
  uint32_t scanAddr = bctrAddr;
  bool foundMtctr = false;
  bool foundLoad = false;

  for (int i = 0; i < kMaxBackwardScan && scanAddr >= containingRegion.start + 4; i++) {
    scanAddr -= 4;
    auto* insn = decoded.get(scanAddr);
    if (!insn)
      break;

    // Stop at unconditional terminators (but NOT conditional branches - they're often bounds
    // checks)
    if (isTerminator(*insn) && !isConditional(*insn))
      break;

    // Find mtctr rX
    if (!foundMtctr) {
      if (insn->opcode == Opcode::mtctr || insn->opcode == Opcode::mtspr) {
        // mtctr is mtspr 9, rS
        ctrSourceReg = insn->XFX.RT;
        foundMtctr = true;
        continue;
      }
    }

    // After mtctr, look for load into ctrSourceReg
    if (foundMtctr && !foundLoad) {
      // lwzx rD, rA, rB - indexed word load (ABSOLUTE table)
      if (insn->opcode == Opcode::lwzx && insn->X.RT == ctrSourceReg) {
        // Table address is in rA, index scaled in rB
        tableType = JumpTableType::kAbsolute;
        indexReg = insn->X.RB;
        finalIndexReg = indexReg;
        foundLoad = true;
        REXCODEGEN_TRACE("detectJumpTable: bctr=0x{:08X} found lwzx at 0x{:08X}", bctrAddr,
                         scanAddr);
        continue;
      }

      // lbzx rD, rA, rB - indexed byte load (BYTE/COMPUTED table)
      if (insn->opcode == Opcode::lbzx && insn->X.RT == ctrSourceReg) {
        // Don't overwrite kComputed (set by rlwinm for shifted byte tables)
        if (tableType != JumpTableType::kComputed) {
          tableType = JumpTableType::kByteOffset;
        }
        indexReg = insn->X.RB;
        finalIndexReg = indexReg;
        foundLoad = true;
        REXCODEGEN_TRACE("detectJumpTable: bctr=0x{:08X} found lbzx at 0x{:08X}", bctrAddr,
                         scanAddr);
        continue;
      }

      // lhzx rD, rA, rB - indexed halfword load (SHORTOFFSET table)
      if (insn->opcode == Opcode::lhzx && insn->X.RT == ctrSourceReg) {
        // Don't overwrite kComputed
        if (tableType != JumpTableType::kComputed) {
          tableType = JumpTableType::kShortOffset;
        }
        indexReg = insn->X.RB;
        finalIndexReg = indexReg;
        foundLoad = true;
        REXCODEGEN_TRACE("detectJumpTable: bctr=0x{:08X} found lhzx at 0x{:08X}", bctrAddr,
                         scanAddr);
        continue;
      }

      // add rD, rA, rB - combining base with offset
      // Pattern: rD = base + offset, where one operand is base, other is from table
      if (insn->opcode == Opcode::add && insn->XO.RT == ctrSourceReg) {
        // If RA == rD (e.g., r12 = r12 + r0), then RB has the table offset
        // If RA != rD (e.g., r12 = r11 + r0), follow RA for the chain
        if (insn->XO.RA == ctrSourceReg) {
          // Pattern: r12 = r12 + r0 --> r0 came from table load
          ctrSourceReg = insn->XO.RB;
        } else {
          // Follow RA
          ctrSourceReg = insn->XO.RA;
        }
        REXCODEGEN_TRACE("detectJumpTable: bctr=0x{:08X} found add at 0x{:08X}, now tracking r{}",
                         bctrAddr, scanAddr, ctrSourceReg);
        continue;
      }

      // rlwinm - shift for computed offset (slwi is rlwinm simplified)
      if (insn->opcode == Opcode::rlwinm && insn->M.RA == ctrSourceReg) {
        shiftAmount = insn->M.SH;
        if (shiftAmount > 0) {
          tableType = JumpTableType::kComputed;
        }
        ctrSourceReg = insn->M.RS;
        REXCODEGEN_TRACE("detectJumpTable: bctr=0x{:08X} found rlwinm at 0x{:08X}", bctrAddr,
                         scanAddr);
        continue;
      }

      // Log unhandled instructions in the chain (potential issue)
      REXCODEGEN_TRACE(
          "detectJumpTable: bctr=0x{:08X} unhandled insn at 0x{:08X} opcode={} while looking for "
          "load into r{}",
          bctrAddr, scanAddr, static_cast<int>(insn->opcode), ctrSourceReg);
    }

    // After load: trace back indexReg through LEFT SHIFT (slwi) instructions only
    // slwi rA, rS, n is rlwinm rA, rS, n, 0, 31-n (MB=0, ME=31-SH)
    // This scales the index for table lookup (e.g., slwi r0, r31, 1 for halfword table)
    // DON'T trace back through extrwi/other rlwinm variants - those transform the value
    // NOTE: SH must be > 0 for a real shift; SH=0 is just a move/no-op (clrlwi r,r,0)
    // IMPORTANT: Stop tracing if another instruction writes to indexReg (breaks the chain)
    if (foundLoad && indexReg != 0xFF) {
      // Check if this instruction writes to indexReg
      bool writesToIndexReg = false;
      uint8_t destReg = 0xFF;

      // Check common instruction forms that write to a register
      if (insn->opcode == Opcode::rlwinm) {
        destReg = insn->M.RA;
      } else if (insn->opcode == Opcode::srawi || insn->opcode == Opcode::sraw ||
                 insn->opcode == Opcode::srw || insn->opcode == Opcode::slw) {
        // X-form shift instructions: destination is RA (bits 11-15)
        destReg = insn->X.RA;
      } else if (insn->opcode == Opcode::lbz || insn->opcode == Opcode::lhz ||
                 insn->opcode == Opcode::lwz || insn->opcode == Opcode::li ||
                 insn->opcode == Opcode::lis || insn->opcode == Opcode::addi) {
        destReg = insn->D.RT;
      } else if (insn->opcode == Opcode::lbzx || insn->opcode == Opcode::lhzx ||
                 insn->opcode == Opcode::lwzx) {
        // X-form load instructions: destination is RT (bits 6-10)
        destReg = insn->X.RT;
      } else if (insn->opcode == Opcode::or_ || insn->opcode == Opcode::and_ ||
                 insn->opcode == Opcode::xor_ || insn->opcode == Opcode::mr) {
        // X-form logical instructions: destination is RA (bits 11-15), NOT RT
        // (RT is RS/source for these instructions)
        destReg = insn->X.RA;
      } else if (insn->opcode == Opcode::add || insn->opcode == Opcode::subf) {
        // XO-form instructions (add, subf)
        destReg = insn->XO.RT;
      }

      if (destReg == indexReg) {
        writesToIndexReg = true;

        // Check for slwi pattern: if it matches, trace back; otherwise stop tracing
        if (insn->opcode == Opcode::rlwinm) {
          uint8_t sh = insn->M.SH;
          uint8_t mb = insn->M.MB;
          uint8_t me = insn->M.ME;
          // Check for slwi pattern: SH>0, MB=0, ME=31-SH
          if (sh > 0 && mb == 0 && me == (31 - sh)) {
            indexReg = insn->M.RS;
            finalIndexReg = indexReg;
            REXCODEGEN_TRACE(
                "detectJumpTable: bctr=0x{:08X} found slwi at 0x{:08X} indexReg now r{}", bctrAddr,
                scanAddr, indexReg);
          } else {
            // Non-slwi rlwinm writes to indexReg, stop tracing
            REXCODEGEN_TRACE(
                "detectJumpTable: bctr=0x{:08X} indexReg r{} overwritten by non-slwi rlwinm at "
                "0x{:08X}, stop tracing",
                bctrAddr, indexReg, scanAddr);
            indexReg = 0xFF;  // Mark as invalid to stop further tracing
          }
        } else {
          // Another instruction writes to indexReg, stop tracing
          REXCODEGEN_TRACE(
              "detectJumpTable: bctr=0x{:08X} indexReg r{} overwritten at 0x{:08X}, stop tracing",
              bctrAddr, indexReg, scanAddr);
          indexReg = 0xFF;  // Mark as invalid to stop further tracing
        }
      }
    }

    // Find lis/addi pairs for table and base addresses
    // When scanning backward for byte offset tables:
    // - BEFORE foundLoad (between mtctr and lbzx): baseAddr
    // - AFTER foundLoad (before lbzx in forward order): tableAddr
    // For absolute tables (lwzx), there's only tableAddr (after foundLoad)
    if (foundMtctr) {
      // lis rD, HI
      if (insn->opcode == Opcode::lis) {
        uint32_t hi = static_cast<uint32_t>(static_cast<uint16_t>(insn->D.d)) << 16;
        REXCODEGEN_TRACE(
            "detectJumpTable: bctr=0x{:08X} found lis at 0x{:08X} hi=0x{:08X} foundLoad={}",
            bctrAddr, scanAddr, hi, foundLoad);
        if (foundLoad) {
          // After load: this is tableAddr (only capture first complete address)
          if (tableAddr == 0) {
            tableAddr =
                hasPendingTableLo ? combineHiLo(hi, pendingTableLo, pendingTableLoIsAddi) : hi;
            hasPendingTableLo = false;
            REXCODEGEN_TRACE("detectJumpTable: bctr=0x{:08X} set tableAddr=0x{:08X}", bctrAddr,
                             tableAddr);
          }
          // Once tableAddr is set, ignore further lis instructions
        } else {
          // Before load (between mtctr and load): this is baseAddr
          if (baseAddr == 0) {
            baseAddr = hasPendingBaseLo ? combineHiLo(hi, pendingBaseLo, pendingBaseLoIsAddi) : hi;
            hasPendingBaseLo = false;
            REXCODEGEN_TRACE("detectJumpTable: bctr=0x{:08X} set baseAddr=0x{:08X}", bctrAddr,
                             baseAddr);
          }
          // Once baseAddr is set, ignore further lis instructions
        }
      }

      // addi rD, rA, LO (ori also possible)
      if (insn->opcode == Opcode::addi || insn->opcode == Opcode::ori) {
        uint32_t lo = static_cast<uint16_t>(insn->D.d);
        bool isAddi = (insn->opcode == Opcode::addi);
        REXCODEGEN_TRACE(
            "detectJumpTable: bctr=0x{:08X} found {} at 0x{:08X} lo=0x{:04X} foundLoad={}",
            bctrAddr, isAddi ? "addi" : "ori", scanAddr, lo, foundLoad);
        if (foundLoad) {
          // After load: this is tableAddr (only capture first complete address)
          if (tableAddr == 0) {
            if (hasPendingTableHi) {
              tableAddr = combineHiLo(pendingTableHi, lo, isAddi);
              hasPendingTableHi = false;
              REXCODEGEN_TRACE("detectJumpTable: bctr=0x{:08X} set tableAddr=0x{:08X} from pending",
                               bctrAddr, tableAddr);
            } else if (!hasPendingTableLo) {
              pendingTableLo = lo;
              pendingTableLoIsAddi = isAddi;
              hasPendingTableLo = true;
              REXCODEGEN_TRACE("detectJumpTable: bctr=0x{:08X} pending tableLo=0x{:04X}", bctrAddr,
                               lo);
            }
          } else if ((tableAddr & 0xFFFF) == 0) {
            // tableAddr has only high bits, add low bits
            tableAddr = combineHiLo(tableAddr, lo, isAddi);
            REXCODEGEN_TRACE("detectJumpTable: bctr=0x{:08X} combined tableAddr=0x{:08X}", bctrAddr,
                             tableAddr);
          }
          // Once tableAddr is fully set, ignore further addi instructions
        } else {
          // Before load: this is baseAddr
          if (baseAddr == 0) {
            if (hasPendingBaseHi) {
              baseAddr = combineHiLo(pendingBaseHi, lo, isAddi);
              hasPendingBaseHi = false;
              REXCODEGEN_TRACE("detectJumpTable: bctr=0x{:08X} set baseAddr=0x{:08X} from pending",
                               bctrAddr, baseAddr);
            } else if (!hasPendingBaseLo) {
              pendingBaseLo = lo;
              pendingBaseLoIsAddi = isAddi;
              hasPendingBaseLo = true;
              REXCODEGEN_TRACE("detectJumpTable: bctr=0x{:08X} pending baseLo=0x{:04X}", bctrAddr,
                               lo);
            }
          } else if ((baseAddr & 0xFFFF) == 0) {
            baseAddr = combineHiLo(baseAddr, lo, isAddi);
            REXCODEGEN_TRACE("detectJumpTable: bctr=0x{:08X} combined baseAddr=0x{:08X}", bctrAddr,
                             baseAddr);
          }
          // Once baseAddr is fully set, ignore further addi instructions
        }
      }
    }
  }

  REXCODEGEN_TRACE(
      "detectJumpTable: bctr=0x{:08X} scan complete: foundMtctr={} foundLoad={} tableAddr=0x{:08X} "
      "baseAddr=0x{:08X}",
      bctrAddr, foundMtctr, foundLoad, tableAddr, baseAddr);

  if (!foundMtctr || !foundLoad || tableAddr == 0) {
    REXCODEGEN_TRACE(
        "detectJumpTable: bctr=0x{:08X} FAILED foundMtctr={} foundLoad={} tableAddr=0x{:08X}",
        bctrAddr, foundMtctr, foundLoad, tableAddr);
    return std::nullopt;
  }

  // For offset-based tables, we need a base address
  if (tableType != JumpTableType::kAbsolute && baseAddr == 0) {
    baseAddr = containingRegion.start;  // Fallback to region start
  }

  // Find bounds
  auto bounds = scanForBounds(decoded, bctrAddr, containingRegion, finalIndexReg, funcStart);
  // If bounds not found (e.g., state machine pattern with forward bounds check),
  // use max entries and let the validation loop determine actual table size
  uint32_t entryCount = bounds.found ? bounds.maxEntries : kMaxTableEntries;

  // Read table entries
  JumpTable jt;
  jt.bctrAddress = bctrAddr;
  jt.tableAddress = tableAddr;
  jt.indexRegister = finalIndexReg;

  REXCODEGEN_TRACE(
      "detectJumpTable: bctr=0x{:08X} reading {} entries from table=0x{:08X} base=0x{:08X} type={}",
      bctrAddr, entryCount, tableAddr, baseAddr, static_cast<int>(tableType));

  for (uint32_t i = 0; i < entryCount; i++) {
    uint32_t target = 0;

    switch (tableType) {
      case JumpTableType::kAbsolute: {
        auto val = decoded.read<uint32_t>(tableAddr + i * 4);
        if (!val) {
          REXCODEGEN_TRACE("detectJumpTable: bctr=0x{:08X} entry[{}] read failed at 0x{:08X}",
                           bctrAddr, i, tableAddr + i * 4);
          break;
        }
        target = *val;
        break;
      }
      case JumpTableType::kByteOffset: {
        auto val = decoded.read<uint8_t>(tableAddr + i);
        if (!val) {
          REXCODEGEN_TRACE("detectJumpTable: bctr=0x{:08X} entry[{}] read failed at 0x{:08X}",
                           bctrAddr, i, tableAddr + i);
          break;
        }
        target = baseAddr + *val;
        REXCODEGEN_TRACE("detectJumpTable: bctr=0x{:08X} entry[{}] offset=0x{:02X} target=0x{:08X}",
                         bctrAddr, i, *val, target);
        break;
      }
      case JumpTableType::kComputed: {
        auto val = decoded.read<uint8_t>(tableAddr + i);
        if (!val)
          break;
        target = baseAddr + (static_cast<uint32_t>(*val) << shiftAmount);
        break;
      }
      case JumpTableType::kShortOffset: {
        auto val = decoded.read<uint16_t>(tableAddr + i * 2);
        if (!val)
          break;
        target = baseAddr + *val;
        break;
      }
    }

    // Validate target is within code region - jump table targets help DEFINE function extent
    // Don't constrain by funcEnd since that's just PDATA which may not include out-of-line code
    if (target == 0 || !containingRegion.contains(target)) {
      // End of valid entries
      REXCODEGEN_TRACE(
          "detectJumpTable: bctr=0x{:08X} entry[{}] target=0x{:08X} invalid (region "
          "0x{:08X}-0x{:08X})",
          bctrAddr, i, target, containingRegion.start, containingRegion.end);
      if (jt.targets.empty())
        return std::nullopt;
      break;
    }

    // Target must be >= function start (can't jump backward past entry point)
    if (target < funcStart) {
      REXCODEGEN_TRACE(
          "detectJumpTable: bctr=0x{:08X} entry[{}] target=0x{:08X} < funcStart=0x{:08X}", bctrAddr,
          i, target, funcStart);
      if (jt.targets.empty())
        return std::nullopt;
      break;
    }

    // Validate target points to valid code, not null padding
    // TODO(tomc): look into this more. what is the expected behavior when the processor executes
    // a null instruction.
    auto targetInsn = decoded.read<uint32_t>(target);
    if (targetInsn && (*targetInsn == 0x00000000 || *targetInsn == 0xFFFFFFFF)) {
      REXCODEGEN_TRACE(
          "detectJumpTable: bctr=0x{:08X} entry[{}] target=0x{:08X} points to null/padding "
          "(0x{:08X})",
          bctrAddr, i, target, *targetInsn);
      jt.targets.push_back(0);  // sentinel, handled in codegen as __builtin_trap()
      continue;
    }

    jt.targets.push_back(target);
  }

  if (jt.targets.empty()) {
    REXCODEGEN_TRACE(
        "detectJumpTable: bctr=0x{:08X} table=0x{:08X} NO VALID TARGETS (funcStart=0x{:08X} "
        "funcEnd=0x{:08X})",
        bctrAddr, tableAddr, funcStart, funcEnd);
    return std::nullopt;
  }

  REXCODEGEN_TRACE("detectJumpTable: bctr=0x{:08X} table=0x{:08X} entries={} funcEnd=0x{:08X}",
                   bctrAddr, tableAddr, jt.targets.size(), funcEnd);
  return jt;
}

//=============================================================================
// Block Discovery
//=============================================================================

BlockDiscoveryResult discoverBlocks(DecodedBinary& decoded, uint32_t entryPoint,
                                    const CodeRegion& containingRegion,
                                    const std::unordered_set<uint32_t>& knownFunctions,
                                    uint32_t pdataSize) {
  BlockDiscoveryResult result;
  std::unordered_set<uint32_t> visited;
  std::unordered_set<uint32_t> blockStarts;
  std::queue<uint32_t> worklist;

  // Function extent - use pdataSize when available
  uint32_t funcEnd = (pdataSize > 0) ? (entryPoint + pdataSize) : containingRegion.end;

  REXCODEGEN_TRACE(
      "discoverBlocks: entry=0x{:08X} pdataSize={} funcEnd=0x{:08X} region=[0x{:08X}-0x{:08X}]",
      entryPoint, pdataSize, funcEnd, containingRegion.start, containingRegion.end);

  // Helper to check if address is within function bounds
  auto isWithinFunction = [&](uint32_t addr) -> bool {
    return addr >= entryPoint && addr < funcEnd;
  };

  // Start with entry point
  worklist.push(entryPoint);
  blockStarts.insert(entryPoint);

  while (!worklist.empty()) {
    uint32_t blockStart = worklist.front();
    worklist.pop();

    if (visited.contains(blockStart))
      continue;
    if (!isWithinFunction(blockStart))
      continue;

    // Linear scan until terminator
    uint32_t addr = blockStart;
    Block block;
    block.base = blockStart;
    block.size = 0;

    while (isWithinFunction(addr)) {
      auto* insn = decoded.get(addr);
      if (!insn) {
        REXCODEGEN_TRACE("discoverBlocks: 0x{:08X} no instruction at addr, breaking", entryPoint);
        break;
      }

      // Mark as visited
      visited.insert(addr);

      // Collect instruction pointer
      result.instructions.push_back(insn);

      // Handle branches
      if (isBranch(*insn)) {
        auto target = getBranchTarget(*insn);

        // Helper: check if target is internal to this function
        // Uses funcEnd (from pdataSize or region) defined at top of function
        auto isInternalTarget = [&](uint32_t t) -> bool {
          // Must be within function bounds
          if (t < entryPoint || t >= funcEnd) {
            return false;
          }
          // Must not be a known function entry (except our own entry point)
          if (t != entryPoint && knownFunctions.contains(t)) {
            return false;
          }
          return true;
        };

        if (isCall(*insn)) {
          // bl - function call
          if (target) {
            // All bl instructions need to be recorded as unresolved branches
            // so they can be resolved to CallEdges during merge phase
            result.unresolvedBranches.push_back({addr, *target, true, false});

            // Track as external call for function discovery
            if (!isInternalTarget(*target)) {
              result.externalCalls.push_back(*target);
            }
          }
          // Calls don't terminate block, fall through
        } else if (isReturn(*insn)) {
          // blr - end of function path
          block.size = addr - blockStart + 4;
          break;
        } else if (insn->opcode == rex::codegen::ppc::Opcode::bcctr) {
          // bctr - try to detect jump table
          REXCODEGEN_TRACE("discoverBlocks: bctr at 0x{:08X} in func 0x{:08X}, funcEnd=0x{:08X}",
                           addr, entryPoint, funcEnd);
          auto jt = detectJumpTable(decoded, addr, containingRegion, entryPoint, funcEnd);
          if (jt) {
            REXCODEGEN_TRACE("discoverBlocks: detected jump table at bctr 0x{:08X} with {} targets",
                             addr, jt->targets.size());
            result.jumpTables.push_back(*jt);
            // Jump table targets are definitionally part of this function
            // Extend funcEnd if any target exceeds it (within region bounds)
            // This handles out-of-line switch case code
            for (uint32_t t : jt->targets) {
              if (t == 0)
                continue;  // sentinel from null-padding detection
              if (t >= funcEnd && t < containingRegion.end) {
                funcEnd = t + 4;  // Extend to include this target
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
          // Conditional branch - follow both paths
          if (target && isInternalTarget(*target)) {
            result.labels.insert(*target);
            if (!visited.contains(*target) && !blockStarts.contains(*target)) {
              blockStarts.insert(*target);
              worklist.push(*target);
            }
          } else if (target) {
            // External conditional branch (or conditional tail call to known function)
            result.unresolvedBranches.push_back({addr, *target, false, true});
          }
          // CRITICAL: Fall-through also needs a label
          uint32_t fallthrough = addr + 4;
          if (isInternalTarget(fallthrough)) {
            result.labels.insert(fallthrough);
            if (!visited.contains(fallthrough) && !blockStarts.contains(fallthrough)) {
              blockStarts.insert(fallthrough);
              worklist.push(fallthrough);
            }
          }
        } else {
          // Unconditional branch
          if (target) {
            if (isInternalTarget(*target)) {
              // Internal unconditional branch (includes backward branches)
              result.labels.insert(*target);
              if (!visited.contains(*target) && !blockStarts.contains(*target)) {
                blockStarts.insert(*target);
                worklist.push(*target);
              }
            } else {
              // Tail call to external
              result.tailCalls.push_back(*target);
              result.unresolvedBranches.push_back({addr, *target, false, false});
            }
          }
          block.size = addr - blockStart + 4;
          break;
        }
      }

      // Check for block terminator (null, prologue of next function, etc.)
      if (isBlockTerminator(*insn, addr, containingRegion, knownFunctions)) {
        REXCODEGEN_TRACE("discoverBlocks: 0x{:08X} block terminator at 0x{:08X}", entryPoint, addr);
        block.size = addr - blockStart + 4;
        break;
      }

      addr += 4;
    }

    // Log why loop exited if not due to terminator
    if (block.size == 0 && !isWithinFunction(addr)) {
      REXCODEGEN_TRACE("discoverBlocks: 0x{:08X} addr 0x{:08X} outside function (funcEnd=0x{:08X})",
                       entryPoint, addr, funcEnd);
    }

    // Finalize block size if not set
    if (block.size == 0) {
      block.size = addr - blockStart;
    }

    if (block.size > 0) {
      result.blocks.push_back(block);
    }
  }

  // Sort blocks by address
  std::sort(result.blocks.begin(), result.blocks.end(),
            [](const Block& a, const Block& b) { return a.base < b.base; });

  // Remove duplicate instructions (in case of overlapping scans)
  std::sort(result.instructions.begin(), result.instructions.end(),
            [](auto* a, auto* b) { return a->address < b->address; });
  result.instructions.erase(std::unique(result.instructions.begin(), result.instructions.end()),
                            result.instructions.end());

  REXCODEGEN_TRACE("discoverBlocks: entry=0x{:08X} blocks={} instructions={} labels={}", entryPoint,
                   result.blocks.size(), result.instructions.size(), result.labels.size());

  return result;
}

}  // namespace rex::codegen
