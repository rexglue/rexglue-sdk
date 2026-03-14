/**
 * @file        codegen/function_scanner.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "codegen_flags.h"
#include "decoded_binary.h"
#include "ppc/instruction.h"
#include "ppc/opcode.h"

#include <algorithm>
#include <queue>
#include <set>
#include <stack>
#include <unordered_set>

#include <fmt/format.h>

#include <rex/codegen/binary_view.h>
#include <rex/codegen/codegen_context.h>
#include <rex/codegen/function_scanner.h>
#include <rex/logging.h>

#include "codegen_logging.h"
#include <rex/memory/utils.h>
#include <rex/types.h>

namespace rex::codegen {

// Import PPC types
using rex::codegen::ppc::decode_instruction;
using rex::codegen::ppc::Instruction;
using rex::codegen::ppc::Opcode;
using rex::memory::load_and_swap;

//=============================================================================
// Constructors
//=============================================================================

FunctionScanner::FunctionScanner(const BinaryView& binary) : binary_(&binary) {}

//=============================================================================
// Address Translation
//=============================================================================

template <typename T>
const T* FunctionScanner::translate_address(rex::guest_addr_t guest_addr) const {
  return reinterpret_cast<const T*>(binary_->translate(static_cast<uint32_t>(guest_addr)));
}

bool FunctionScanner::isExecutableSection(rex::guest_addr_t address) const {
  return binary_->isExecutable(static_cast<uint32_t>(address));
}

//=============================================================================
// Helper: Detect function prologue pattern
//=============================================================================

bool FunctionScanner::is_prologue_pattern(guest_addr_t address) {
  auto host_ptr = translate_address<u32>(address);
  if (!host_ptr)
    return false;

  u32 code = load_and_swap<u32>(host_ptr);
  Instruction instr = decode_instruction(address, code);

  // Check for mflr
  if (instr.opcode == Opcode::mflr) {
    return true;
  }

  // Check for mfspr lr (SPR 8)
  if (instr.opcode == Opcode::mfspr && instr.XFX.spr_num() == 8) {
    return true;
  }

  // Check for stack frame allocation: stwu r1, -X(r1)
  if (instr.opcode == Opcode::stwu && instr.D.RS() == 1 && instr.D.RA == 1 && instr.D.SIMM() < 0) {
    return true;
  }

  return false;
}

//=============================================================================
// Helper: Detect function epilogue pattern
//=============================================================================

bool FunctionScanner::is_epilogue_pattern(guest_addr_t address) {
  auto host_ptr = translate_address<u32>(address);
  if (!host_ptr)
    return false;

  u32 code = load_and_swap<u32>(host_ptr);

  Instruction instr = decode_instruction(address, code);

  // Check for blr
  if (instr.is_return()) {
    return true;
  }

  // Check for mtlr
  if (instr.opcode == Opcode::mtlr) {
    return true;
  }

  // Check for stack restore: lwz r1, 0(r1)
  if (instr.opcode == Opcode::lwz && instr.D.RT == 1 && instr.D.RA == 1 && instr.D.SIMM() == 0) {
    return true;
  }

  return false;
}

//=============================================================================
// Helper: Detect save/restore helper functions via byte pattern matching
//=============================================================================
bool FunctionScanner::is_restgprlr_function(guest_addr_t address) {
  auto host_ptr = translate_address<u32>(address);
  if (!host_ptr)
    return false;

  u32 first = load_and_swap<u32>(host_ptr);

  // Check single-instruction patterns (4 bytes each)
  constexpr u32 RESTGPRLR_14 = 0xe9c1ff68;  // ld r14, -0x98(r1)
  constexpr u32 SAVEGPRLR_14 = 0xf9c1ff68;  // std r14, -0x98(r1)
  constexpr u32 RESTFPR_14 = 0xc9ccff70;    // lfd f14, -0x90(r12)
  constexpr u32 SAVEFPR_14 = 0xd9ccff70;    // stfd f14, -0x90(r12)

  if (first == RESTGPRLR_14 || first == SAVEGPRLR_14 || first == RESTFPR_14 ||
      first == SAVEFPR_14) {
    return true;
  }

  // Check two-instruction patterns (8 bytes each)
  // Pattern: li r11, -0x120 (0x3960fee0) + lvx/stvx
  if (first == 0x3960fee0) {  // li r11, -0x120
    auto second_ptr = translate_address<u32>(address + 4);
    if (second_ptr) {
      u32 second = load_and_swap<u32>(second_ptr);
      constexpr u32 RESTVMX_14 = 0x7dcb60ce;  // lvx v14, r11, r12
      constexpr u32 SAVEVMX_14 = 0x7dcb61ce;  // stvx v14, r11, r12
      if (second == RESTVMX_14 || second == SAVEVMX_14) {
        return true;
      }
    }
  }

  // Pattern: li r11, -0x400 (0x3960fc00) + lvx128/stvx128
  if (first == 0x3960fc00) {  // li r11, -0x400
    auto second_ptr = translate_address<u32>(address + 4);
    if (second_ptr) {
      u32 second = load_and_swap<u32>(second_ptr);
      constexpr u32 RESTVMX_64 = 0x100b60cb;  // lvx128 v64, r11, r12
      constexpr u32 SAVEVMX_64 = 0x100b61cb;  // stvx128 v64, r11, r12
      if (second == RESTVMX_64 || second == SAVEVMX_64) {
        return true;
      }
    }
  }

  return false;
}

//=============================================================================
// Jump Table Pattern Detection
//=============================================================================
// Xbox 360 compilers emit 4 distinct jump table patterns (maybe more?):
//
// 1. ABSOLUTE: lwzx loads full 32-bit target addresses
//    lis rT, table@ha; addi rT, rT, table@l; rlwinm rI, rIdx, 2; lwzx rT, rI, rT; mtctr; bctr
//
// 2. COMPUTED: lbzx loads byte offset, shifted and added to base
//    lis rT, table@ha; addi rT, rT, table@l; lbzx rO, rIdx, rT; rlwinm rO, rO, shift;
//    lis rB, base@ha; addi rB, rB, base@l; add rT, rB, rO; mtctr; bctr
//
// 3. BYTEOFFSET: lbzx loads byte offset, added directly to base
//    lis rT, table@ha; addi rT, rT, table@l; lbzx rO, rIdx, rT;
//    lis rB, base@ha; addi rB, rB, base@l; add rT, rB, rO; mtctr; bctr
//
// 4. SHORTOFFSET: lhzx loads 16-bit offset, added to base
//    lis rT, table@ha; addi rT, rT, table@l; rlwinm rI, rIdx, 1; lhzx rO, rI, rT;
//    lis rB, base@ha; addi rB, rB, base@l; add rT, rB, rO; mtctr; bctr
//=============================================================================

namespace {

// Jump table type - internal use only during detection
// Determines how target addresses are stored/computed in the binary
enum class JumpTableType : u8 {
  kAbsolute,     // lwzx - table contains full 32-bit target addresses
  kComputed,     // lbzx + rlwinm + add - byte offset shifted and added to base
  kByteOffset,   // lbzx + add - byte offset added directly to base
  kShortOffset,  // lhzx + add - 16-bit offset added to base
};

// Helper struct for tracking pattern match state
struct JumpTableMatch {
  JumpTableType type = JumpTableType::kAbsolute;
  u8 ctr_source_reg = 0;  // Register moved to CTR
  u8 table_reg = 0;       // Register holding table address
  u8 base_reg = 0;        // Register holding base address (offset types)
  u8 index_reg = 0;       // Register holding original switch index
  u8 offset_reg = 0;      // Register holding loaded offset
  u8 shift_amount = 0;    // Shift amount for COMPUTED type
  guest_addr_t table_high = 0;
  guest_addr_t table_low = 0;
  guest_addr_t base_high = 0;
  guest_addr_t base_low = 0;

  // Pattern matching state
  bool found_mtctr = false;
  bool found_add = false;    // For offset-based types
  bool found_load = false;   // lwzx/lbzx/lhzx
  bool found_shift = false;  // rlwinm for shift
  bool found_table_lis = false;
  bool found_table_addi = false;
  bool found_base_lis = false;
  bool found_base_addi = false;

  // For @ha/@l pairs, addi uses signed immediate, so we need addition with sign extension
  guest_addr_t table_address() const { return table_high + static_cast<i16>(table_low); }
  guest_addr_t base_address() const { return base_high + static_cast<i16>(base_low); }
};

// Scan backward for CMPLWI bounds check and conditional branch
struct BoundsInfo {
  uint32_t maxEntries = 0;
  uint8_t indexReg = 0;
  uint32_t defaultTarget = 0;
  bool found = false;
};

//=============================================================================
// Helper: Scan for bounds check (CMPLWI + BGT/BLE)
//=============================================================================

BoundsInfo scanForBounds(const FunctionScanner& scanner, guest_addr_t bctr_address,
                         u8 expected_index_reg) {
  BoundsInfo bounds;
  constexpr int MAX_SCAN = 64;

  u8 cr_field = 0xFF;  // CR field from conditional branch

  for (int i = 1; i <= MAX_SCAN; ++i) {
    guest_addr_t addr = bctr_address - (i * 4);
    if (addr < 4)
      break;

    auto host_ptr = scanner.translate_address<u32>(addr);
    if (!host_ptr)
      break;

    u32 code = load_and_swap<u32>(host_ptr);
    if (code == 0)
      break;

    Instruction instr = decode_instruction(addr, code);

    // Look for conditional branch (bc, bca, bcl, bcla, bclr, bclrl)
    // bgt/ble/bgtlr/blelr are simplified mnemonics for bc with specific BO/BI
    if (cr_field == 0xFF) {
      bool is_cond_branch = (instr.opcode == Opcode::bc || instr.opcode == Opcode::bca ||
                             instr.opcode == Opcode::bcl || instr.opcode == Opcode::bcla ||
                             instr.opcode == Opcode::bclr || instr.opcode == Opcode::bclrl);

      if (is_cond_branch) {
        // Check if this could be a bounds check branch (bgt or ble pattern)
        // BO[4] (bit 0) = 0 means test CR bit
        // BI[0:1] = condition bit within CR field (GT=1, LT=0, EQ=2, SO=3)
        u8 bi = instr.B.BI;

        // Check for branches that test the GT bit (BI mod 4 == 1)
        // bgt: BO=12 (01100), tests CR[GT]=true
        // ble: BO=4 (00100), tests CR[GT]=false (i.e., not greater = less or equal)
        if ((bi & 0x3) == 1) {  // Tests GT bit
          // Extract CR field (bits 2-4 of BI)
          cr_field = (bi >> 2) & 0x7;

          // Extract default target if branch has target
          if (instr.branch_target.has_value()) {
            bounds.defaultTarget = instr.branch_target.value();
          }
        }
      }
    }

    // Look for rlwinm that sets the index register with a mask
    // clrlwi rD, rS, n = rlwinm rD, rS, 0, n, 31
    // This bounds the value to 2^(32-n) - 1 entries
    if (instr.opcode == Opcode::rlwinm && instr.M.RA == expected_index_reg) {
      u8 sh = instr.M.SH;
      u8 mb = instr.M.MB;
      u8 me = instr.M.ME;

      // clrlwi pattern: SH=0, ME=31, MB > 0
      // The mask clears leftmost MB bits, so max value = 2^(32-MB) - 1
      // Entry count = max value + 1 = 2^(32-MB)
      if (sh == 0 && me == 31 && mb > 0 && mb < 32) {
        u32 implicit_count = 1u << (32 - mb);
        REXCODEGEN_TRACE(
            "  [0x{:08X}] Found clrlwi/rlwinm r{}, ..., {} -> {} entries (implicit mask)", addr,
            static_cast<u32>(expected_index_reg), mb, implicit_count);

        // Only accept if count is reasonable (2-256 entries)
        if (implicit_count >= 2 && implicit_count <= 256) {
          bounds.maxEntries = implicit_count;
          bounds.indexReg = expected_index_reg;
          bounds.found = true;
          break;  // Implicit bounds are definitive
        }
      }
    }

    // Look for cmpli or cmpi (cmplwi/cmpwi - unsigned/signed bounds check)
    if (instr.opcode == Opcode::cmpli || instr.opcode == Opcode::cmpi) {
      // cmpli format: cmpli BF, L, RA, UIMM
      // BF (bits 23-25): CR field
      // L (bit 21): 0=32-bit, 1=64-bit
      // RA (bits 16-20): Register to compare
      // UIMM (bits 0-15): Immediate value

      // BF is top 3 bits of RT field for compare instructions
      u8 cmp_cr = instr.D.RT >> 2;
      u8 cmp_ra = instr.D.RA;        // Register being compared
      u16 cmp_imm = instr.D.UIMM();  // Immediate value (max index)

      // Prefer register match, accept CR-only match as fallback
      bool cr_matches = (cr_field != 0xFF && cmp_cr == cr_field);
      bool reg_matches = (cmp_ra == expected_index_reg);

      // CRITICAL: Reject very small immediates (0 or 1) even if register matches
      // These are likely unrelated comparisons (checking for zero/null, boolean tests)
      // A real switch table bounds check would have immediate >= 2 (at least 3 cases)
      if (cmp_imm <= 1) {
        REXCODEGEN_TRACE(
            "  [0x{:08X}] Skipping cmpli r{}, {} (immediate too small for switch bounds)", addr,
            static_cast<u32>(cmp_ra), cmp_imm);
        continue;
      }

      // Only accept if register matches, or if CR matches AND immediate is reasonable
      if (reg_matches || (cr_matches && cmp_imm > 1)) {
        bounds.maxEntries = cmp_imm + 1;  // cmpli compares against max, so count = max + 1
        bounds.indexReg = cmp_ra;
        bounds.found = true;

        // If register matches, we found the best match - done
        if (reg_matches)
          break;
        // If only CR matches, continue scanning for a better (register) match
      }
    }
  }

  return bounds;
}

//=============================================================================
// Helper: Read table entries based on type
//=============================================================================
std::vector<guest_addr_t> read_table_entries(const FunctionScanner& scanner,
                                             const JumpTableMatch& match, u32 entry_count) {
  std::vector<guest_addr_t> targets;
  guest_addr_t table_addr = match.table_address();

  // entry_count comes from bounds check analysis - use it exactly if available
  // If no bounds check was found (entry_count == 0), read until we hit invalid entries
  // Loop terminates via goto done when invalid memory is hit
  for (u32 i = 0; entry_count == 0 || i < entry_count; ++i) {
    guest_addr_t target = 0;

    switch (match.type) {
      case JumpTableType::kAbsolute: {
        // Read 32-bit address directly (big-endian)
        auto entry_ptr = scanner.translate_address<u32>(table_addr + (i * 4));
        if (!entry_ptr)
          goto done;
        target = load_and_swap<u32>(entry_ptr);
        break;
      }

      case JumpTableType::kComputed: {
        // Read byte, shift, add to base
        auto entry_ptr = scanner.translate_address<u8>(table_addr + i);
        if (!entry_ptr)
          goto done;
        u8 offset = *entry_ptr;
        target = match.base_address() + (static_cast<u32>(offset) << match.shift_amount);
        break;
      }

      case JumpTableType::kByteOffset: {
        // Read byte, add directly to base
        auto entry_ptr = scanner.translate_address<u8>(table_addr + i);
        if (!entry_ptr)
          goto done;
        u8 offset = *entry_ptr;
        target = match.base_address() + offset;
        break;
      }

      case JumpTableType::kShortOffset: {
        // Read 16-bit value (big-endian), add to base
        auto entry_ptr = scanner.translate_address<u16>(table_addr + (i * 2));
        if (!entry_ptr)
          goto done;
        u16 offset = load_and_swap<u16>(entry_ptr);
        target = match.base_address() + offset;
        break;
      }
    }

    // PPC instructions must be 4-byte aligned
    if (target & 3)
      goto done;

    // Validate target (stop on null or invalid for absolute type)
    if (target == 0 && match.type == JumpTableType::kAbsolute)
      goto done;

    // Validate target is in EXECUTABLE section (not just mapped memory)
    // This prevents false positives from data tables containing addresses
    if (!scanner.isExecutableSection(target)) {
      // For absolute tables, non-executable target means wrong table address
      if (match.type == JumpTableType::kAbsolute)
        goto done;
      continue;  // Skip non-executable entries in offset tables (might be default/error cases)
    }

    targets.push_back(target);
  }

done:
  return targets;
}

}  // anonymous namespace

//=============================================================================
// Helper: Check if instruction indicates a function boundary
//=============================================================================
// Returns true if the instruction at 'code' indicates we've crossed into
// a different function (either previous function's terminator or current
// function's prologue).
static bool is_function_boundary(u32 code, const Instruction& instr, guest_addr_t addr) {
  // Zero padding between functions
  if (code == 0x00000000) {
    REXCODEGEN_TRACE("  [0x{:08X}] Hit zero padding - function boundary", addr);
    return true;
  }

  // blr - previous function's return
  if (instr.is_return()) {
    REXCODEGEN_TRACE("  [0x{:08X}] Hit blr - function boundary", addr);
    return true;
  }

  // bctr/bctrl - indirect branch/call via CTR
  if (instr.opcode == Opcode::bcctr || instr.opcode == Opcode::bcctrl) {
    REXCODEGEN_TRACE("  [0x{:08X}] Hit bctr/bctrl - function boundary", addr);
    return true;
  }

  // Unconditional branch 'b' (tail call to named function)
  if (instr.opcode == Opcode::b || instr.opcode == Opcode::ba) {
    REXCODEGEN_TRACE("  [0x{:08X}] Hit unconditional branch (b) - function boundary", addr);
    return true;
  }

  // mflr - function prologue (saving link register)
  if (instr.opcode == Opcode::mflr) {
    REXCODEGEN_TRACE("  [0x{:08X}] Hit mflr - function prologue", addr);
    return true;
  }

  // stw rX, offset(r1) where offset is negative - stack frame setup
  // This catches "stwu r1, -N(r1)" which is a common prologue
  if (instr.opcode == Opcode::stwu && instr.D.RA == 1 && instr.D.RT == 1) {
    // stwu r1, -N(r1) is stack frame allocation
    REXCODEGEN_TRACE("  [0x{:08X}] Hit stwu r1 (stack frame) - function prologue", addr);
    return true;
  }

  // nop (ori r0,r0,0 = 0x60000000) - often used as padding
  // But nops can appear mid-function too, so only treat consecutive nops as boundary
  // For now, don't treat single nop as boundary

  return false;
}

//=============================================================================
// Helper: Detect jump table pattern at bctr instruction
//=============================================================================
std::optional<JumpTable> FunctionScanner::detect_jump_table(guest_addr_t bctr_address) {
  // Skip detection if this address has a manually-specified switch table
  if (known_switch_tables_.count(static_cast<uint32_t>(bctr_address))) {
    REXCODEGEN_TRACE("detect_jump_table: skipping 0x{:08X} (manual table exists)", bctr_address);
    return std::nullopt;  // Will be handled by the pre-loaded config
  }

  const int MAX_SCAN_BACK = static_cast<int>(REXCVAR_GET(backward_scan_limit));

  JumpTableMatch match;

  REXCODEGEN_TRACE("detect_jump_table: scanning backward from bctr at 0x{:08X}", bctr_address);

  // Backward scan to match pattern
  for (int i = 1; i <= MAX_SCAN_BACK; ++i) {
    guest_addr_t addr = bctr_address - (i * 4);
    if (addr < 4)
      break;

    auto host_ptr = translate_address<u32>(addr);
    if (!host_ptr)
      break;

    // Read as big-endian (PPC is big-endian)
    u32 code = load_and_swap<u32>(host_ptr);

    Instruction instr = decode_instruction(addr, code);

    // Stop at function boundaries - but allow continuing past bctr if we're still
    // looking for lis (handles adjacent switch tables sharing setup code)
    if (is_function_boundary(code, instr, addr)) {
      // If we found load but not lis, and this is bctr, continue scanning
      // (adjacent switch tables may share the same lis setup)
      if (instr.opcode == Opcode::bcctr && match.found_load && !match.found_table_lis) {
        REXCODEGEN_TRACE("  [0x{:08X}] Continuing past bctr to find shared lis", addr);
        continue;
      }
      break;
    }

    // Step 1: Find mtctr rX
    if (!match.found_mtctr && instr.opcode == Opcode::mtctr) {
      match.ctr_source_reg = instr.XFX.RS();
      match.found_mtctr = true;
      REXCODEGEN_TRACE("  [0x{:08X}] Found mtctr r{}", addr,
                       static_cast<u32>(match.ctr_source_reg));
      continue;
    }

    if (!match.found_mtctr)
      continue;

    // Step 2a: Find add rT, rBase, rOffset (for offset-based types)
    if (!match.found_add && !match.found_load && instr.opcode == Opcode::add) {
      if (instr.XO.RT == match.ctr_source_reg) {
        // Store both RA and RB - we'll determine which is base/offset later
        match.base_reg = instr.XO.RA;
        match.offset_reg = instr.XO.RB;
        match.found_add = true;
        REXCODEGEN_TRACE("  [0x{:08X}] Found add r{}, r{}, r{}", addr,
                         static_cast<u32>(instr.XO.RT), static_cast<u32>(match.base_reg),
                         static_cast<u32>(match.offset_reg));
        continue;
      }
    }

    // Step 2b: Find lwzx (ABSOLUTE type) - only if no add found
    // lwzx RT, RA, RB: RT = mem[RA + RB]
    // Note: RA and RB can be in either order (table/index or index/table)
    if (!match.found_add && !match.found_load && instr.opcode == Opcode::lwzx) {
      if (instr.X.RT == match.ctr_source_reg) {
        match.type = JumpTableType::kAbsolute;
        // Initially assume RA=table, RB=index (will verify/swap later)
        match.table_reg = instr.X.RA;
        match.index_reg = instr.X.RB;
        match.found_load = true;
        REXCODEGEN_TRACE("  [0x{:08X}] Found lwzx r{}, r{}, r{} (tentative table=r{}, index=r{})",
                         addr, static_cast<u32>(instr.X.RT), static_cast<u32>(instr.X.RA),
                         static_cast<u32>(instr.X.RB), static_cast<u32>(match.table_reg),
                         static_cast<u32>(match.index_reg));
        continue;
      }
    }

    // For offset-based types: find the load instruction
    if (match.found_add && !match.found_load) {
      // lbzx for COMPUTED or BYTEOFFSET
      // lbzx RT, RA, RB: RT = *(RA + RB)
      if (instr.opcode == Opcode::lbzx && instr.X.RT == match.offset_reg) {
        // Initially assume RA=table, RB=index
        match.table_reg = instr.X.RA;
        match.index_reg = instr.X.RB;
        match.found_load = true;
        // Only set type if we haven't already found a shift (which means kComputed)
        if (!match.found_shift) {
          match.type = JumpTableType::kByteOffset;  // Default when no shift
        }
        REXCODEGEN_TRACE("  [0x{:08X}] Found lbzx r{}, r{}, r{}", addr,
                         static_cast<u32>(instr.X.RT), static_cast<u32>(instr.X.RA),
                         static_cast<u32>(instr.X.RB));
        continue;
      }

      // lhzx for SHORTOFFSET
      if (instr.opcode == Opcode::lhzx && instr.X.RT == match.offset_reg) {
        match.type = JumpTableType::kShortOffset;
        match.table_reg = instr.X.RA;
        match.index_reg = instr.X.RB;
        match.found_load = true;
        REXCODEGEN_TRACE("  [0x{:08X}] Found lhzx r{}, r{}, r{}", addr,
                         static_cast<u32>(instr.X.RT), static_cast<u32>(instr.X.RA),
                         static_cast<u32>(instr.X.RB));
        continue;
      }
    }

    // Step 3: Find rlwinm for shift (COMPUTED type or index scaling)
    if (!match.found_shift && instr.opcode == Opcode::rlwinm) {
      // Check if this is scaling the index (for ABSOLUTE, SHORT, or offset-based types)
      // Must check BEFORE offset shift to handle SHORTOFFSET with scaled index
      if (match.found_load && instr.M.RA == match.index_reg) {
        match.index_reg = instr.M.RS;
        match.found_shift = true;
        REXCODEGEN_TRACE("  [0x{:08X}] Found rlwinm (index scale) r{}, r{}, {}", addr,
                         static_cast<u32>(instr.M.RA), static_cast<u32>(instr.M.RS),
                         static_cast<u32>(instr.M.SH));
        continue;
      }
      // Check if this is shifting the offset (COMPUTED type)
      // Only set COMPUTED type if we haven't already identified as SHORTOFFSET
      // (SHORTOFFSET uses rlwinm for index scaling, not offset shifting)
      if (match.found_add && instr.M.RA == match.offset_reg &&
          match.type != JumpTableType::kShortOffset) {
        match.shift_amount = instr.M.SH;
        match.type = JumpTableType::kComputed;
        match.found_shift = true;
        match.offset_reg = instr.M.RS;
        REXCODEGEN_TRACE("  [0x{:08X}] Found rlwinm (shift) r{}, r{}, {} -> COMPUTED type", addr,
                         static_cast<u32>(instr.M.RA), static_cast<u32>(instr.M.RS),
                         static_cast<u32>(instr.M.SH));
        continue;
      }
    }

    // Step 4: Find lis/addi pairs for table address (and base for register reuse case)
    // Also check if we need to swap table_reg/index_reg (RA/RB ambiguity)
    if (match.found_load) {
      // Check for register reuse: same register used for both table and base (offset-based types)
      // In this case, backward scan finds BASE first (closer to bctr), then TABLE
      bool register_reuse = match.found_add && (match.table_reg == match.base_reg);

      // Check for lis matching table_reg
      if (instr.opcode == Opcode::lis && instr.D.RT == match.table_reg) {
        if (register_reuse) {
          // Register reuse: first lis = BASE, second lis = TABLE
          if (!match.found_base_lis) {
            match.base_high = static_cast<guest_addr_t>(instr.D.UIMM()) << 16;
            match.found_base_lis = true;
            REXCODEGEN_TRACE("  [0x{:08X}] Found base lis r{}, 0x{:04X} (register reuse)", addr,
                             static_cast<u32>(instr.D.RT), instr.D.UIMM());
            continue;
          } else if (!match.found_table_lis) {
            match.table_high = static_cast<guest_addr_t>(instr.D.UIMM()) << 16;
            match.found_table_lis = true;
            REXCODEGEN_TRACE("  [0x{:08X}] Found table lis r{}, 0x{:04X} (register reuse)", addr,
                             static_cast<u32>(instr.D.RT), instr.D.UIMM());
            continue;
          }
        } else if (!match.found_table_lis) {
          match.table_high = static_cast<guest_addr_t>(instr.D.UIMM()) << 16;
          match.found_table_lis = true;
          REXCODEGEN_TRACE("  [0x{:08X}] Found lis r{}, 0x{:04X}", addr,
                           static_cast<u32>(instr.D.RT), instr.D.UIMM());
          continue;
        }
      }
      // Check if lis matches index_reg - means we guessed wrong, need to swap
      else if (!match.found_table_lis && instr.opcode == Opcode::lis &&
               instr.D.RT == match.index_reg) {
        REXCODEGEN_TRACE("  [0x{:08X}] Found lis for index_reg r{}, swapping table/index", addr,
                         static_cast<u32>(instr.D.RT));
        std::swap(match.table_reg, match.index_reg);
        match.table_high = static_cast<guest_addr_t>(instr.D.UIMM()) << 16;
        match.found_table_lis = true;
        continue;
      }

      // Check for addi/ori matching table_reg
      if ((instr.opcode == Opcode::addi || instr.opcode == Opcode::ori) &&
          instr.D.RT == match.table_reg) {
        if (register_reuse) {
          // Register reuse: first addi = BASE, second addi = TABLE
          if (!match.found_base_addi) {
            match.base_low = instr.D.UIMM();
            match.found_base_addi = true;
            REXCODEGEN_TRACE("  [0x{:08X}] Found base addi r{}, 0x{:04X} (register reuse)", addr,
                             static_cast<u32>(instr.D.RT), instr.D.UIMM());
            continue;
          } else if (!match.found_table_addi) {
            match.table_low = instr.D.UIMM();
            match.found_table_addi = true;
            REXCODEGEN_TRACE("  [0x{:08X}] Found table addi r{}, 0x{:04X} (register reuse)", addr,
                             static_cast<u32>(instr.D.RT), instr.D.UIMM());
            continue;
          }
        } else if (!match.found_table_addi) {
          match.table_low = instr.D.UIMM();
          match.found_table_addi = true;
          REXCODEGEN_TRACE("  [0x{:08X}] Found addi r{}, r{}, 0x{:04X}", addr,
                           static_cast<u32>(instr.D.RT), static_cast<u32>(instr.D.RA),
                           instr.D.UIMM());
          continue;
        }
      }
      // Check if addi matches index_reg - means we guessed wrong, need to swap
      else if (!match.found_table_addi &&
               (instr.opcode == Opcode::addi || instr.opcode == Opcode::ori) &&
               instr.D.RT == match.index_reg) {
        REXCODEGEN_TRACE("  [0x{:08X}] Found addi for index_reg r{}, swapping table/index", addr,
                         static_cast<u32>(instr.D.RT));
        std::swap(match.table_reg, match.index_reg);
        match.table_low = instr.D.UIMM();
        match.found_table_addi = true;
        continue;
      }
    }

    // Step 5: Find lis/addi pairs for base address (offset-based types)
    // Also handle RA/RB ambiguity for add instruction
    if (match.found_add) {
      if (!match.found_base_lis && instr.opcode == Opcode::lis) {
        if (instr.D.RT == match.base_reg) {
          match.base_high = static_cast<guest_addr_t>(instr.D.UIMM()) << 16;
          match.found_base_lis = true;
          REXCODEGEN_TRACE("  [0x{:08X}] Found base lis r{}, 0x{:04X}", addr,
                           static_cast<u32>(instr.D.RT), instr.D.UIMM());
          continue;
        }
        // Check if lis matches offset_reg - means we guessed wrong
        else if (instr.D.RT == match.offset_reg && !match.found_base_lis) {
          REXCODEGEN_TRACE("  [0x{:08X}] Found lis for offset_reg r{}, swapping base/offset", addr,
                           static_cast<u32>(instr.D.RT));
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
          REXCODEGEN_TRACE("  [0x{:08X}] Found base addi r{}, 0x{:04X}", addr,
                           static_cast<u32>(instr.D.RT), instr.D.UIMM());
          continue;
        }
        // Check if addi matches offset_reg - means we guessed wrong
        else if (instr.D.RT == match.offset_reg && !match.found_base_addi) {
          REXCODEGEN_TRACE("  [0x{:08X}] Found addi for offset_reg r{}, swapping base/offset", addr,
                           static_cast<u32>(instr.D.RT));
          std::swap(match.base_reg, match.offset_reg);
          match.base_low = instr.D.UIMM();
          match.found_base_addi = true;
          continue;
        }
      }
    }

    // Check if we have a complete pattern
    bool table_complete = match.found_table_lis && match.found_table_addi;
    bool base_complete = !match.found_add || (match.found_base_lis && match.found_base_addi);

    if (match.found_mtctr && match.found_load && table_complete && base_complete) {
      REXCODEGEN_TRACE("  Pattern complete at 0x{:08X}", addr);
      break;  // Pattern complete
    }
  }

  // Verify minimum required pattern elements
  if (!match.found_mtctr || !match.found_load || !match.found_table_lis ||
      !match.found_table_addi) {
    REXCODEGEN_TRACE("  Pattern incomplete: mtctr={}, load={}, table_lis={}, table_addi={}",
                     match.found_mtctr, match.found_load, match.found_table_lis,
                     match.found_table_addi);
    // Only report error if we found indexed load AND lis/addi for the table address.
    // If we found load but NO lis, it's a vtable/indirect call (runtime pointer), not a switch
    // table.
    if (match.found_load && (match.found_table_lis || match.found_base_lis)) {
      REXCODEGEN_ERROR(
          "Jump table detection failed at bctr 0x{:08X}: mtctr={}, load={}, table_lis={}, "
          "table_addi={}, table_reg=r{}, base_lis={}, base_addi={}",
          bctr_address, match.found_mtctr, match.found_load, match.found_table_lis,
          match.found_table_addi, match.table_reg, match.found_base_lis, match.found_base_addi);
    } else if (match.found_load) {
      // Load but no lis = vtable/indirect call with runtime pointer, not a switch table
      REXCODEGEN_TRACE("bctr 0x{:08X}: indexed load without lis - treating as vtable/indirect call",
                       bctr_address);
    }
    return std::nullopt;
  }

  // For offset-based types, require base address
  if (match.found_add && (!match.found_base_lis || !match.found_base_addi)) {
    REXCODEGEN_TRACE("  Offset-based pattern incomplete: base_lis={}, base_addi={}",
                     match.found_base_lis, match.found_base_addi);
    return std::nullopt;
  }

  // Validate table address
  guest_addr_t table_address = match.table_address();
  REXCODEGEN_TRACE("  Table address: 0x{:08X} (high=0x{:08X}, low=0x{:04X})", table_address,
                   match.table_high, match.table_low);

  auto table_ptr = translate_address<u8>(table_address);
  if (!table_ptr) {
    REXCODEGEN_TRACE("  Invalid table address 0x{:08X} - not in mapped memory", table_address);
    return std::nullopt;
  }

  // Scan for bounds check (CMPLWI)
  BoundsInfo bounds = scanForBounds(*this, bctr_address, match.index_reg);
  REXCODEGEN_TRACE("  Bounds check: found={}, count={}, default=0x{:08X}, index_reg=r{}",
                   bounds.found, bounds.maxEntries, bounds.defaultTarget, bounds.indexReg);

  // Read table entries
  std::vector<guest_addr_t> targets = read_table_entries(*this, match, bounds.maxEntries);

  // Require at least 2 entries
  if (targets.size() < 2) {
    REXCODEGEN_TRACE("  Insufficient entries: {} (need at least 2)", targets.size());
    return std::nullopt;
  }

  // Build result
  JumpTable jt;
  jt.bctrAddress = static_cast<uint32_t>(bctr_address);
  jt.tableAddress = static_cast<uint32_t>(table_address);
  jt.indexRegister = match.index_reg;
  jt.targets = std::move(targets);

  return jt;
}

//=============================================================================
// Block-Based Discovery
//=============================================================================
FunctionBlocks FunctionScanner::discover_blocks(rex::guest_addr_t entry_point,
                                                rex::u32 pdata_size) {
  FunctionBlocks result;
  result.entry = entry_point;
  result.pdata_size = pdata_size;

  // Track all instruction addresses scanned (prevents overlap between blocks)
  std::unordered_set<guest_addr_t> scannedAddrs;

  // DFS block stack - tracks blocks being processed
  // this allows projection carry-forward on continuous blocks
  std::vector<DiscoveredBlock> block_stack;

  // Start with entry block
  DiscoveredBlock entry_block;
  entry_block.base = entry_point;
  entry_block.end = entry_point;
  entry_block.projectedSize = -1;  // No limit
  block_stack.push_back(entry_block);

  const size_t MAX_BLOCKS = REXCVAR_GET(max_blocks_per_function);  // Safety limit

  while (!block_stack.empty() && result.blocks.size() < MAX_BLOCKS) {
    // Get current block from stack (by reference for in-place modification)
    DiscoveredBlock& block = block_stack.back();

    // Only check for duplicates if this is a FRESH block (not partially scanned)
    // When block.end > block.base, we're continuing to process an existing block
    if (block.end == block.base) {
      // Fresh block - check if already scanned by another block
      if (scannedAddrs.count(block.base)) {
        block_stack.pop_back();
        continue;
      }
    }

    // Validate alignment
    if ((block.base & 0x3) != 0) {
      REXCODEGEN_WARN("discover_blocks: misaligned block start 0x{:08X}", block.base);
      block_stack.pop_back();
      continue;
    }

    // Calculate current position in block
    guest_addr_t addr = block.end;  // end tracks where we are (exclusive becomes next addr)
    if (addr == block.base) {
      // Fresh block - start scanning from base
    }

    // Check projection limit BEFORE processing instruction
    // if block.size >= projectedSize, block is done
    guest_addr_t block_size = addr - block.base;
    if (block.projectedSize != -1 && block_size >= static_cast<guest_addr_t>(block.projectedSize)) {
      REXCODEGEN_TRACE("Block 0x{:08X} hit projection limit at size 0x{:X}", block.base,
                       block_size);
      // Block done - save and pop
      result.blocks.push_back(block);
      block_stack.pop_back();
      continue;
    }

    // Check if this address was already scanned by another block (overlap prevention)
    // This catches cases where a fall-through block tries to scan into territory
    // already covered by another block (e.g., shared epilogue code)
    if (scannedAddrs.count(addr)) {
      // Block ends here - don't include the already-scanned instruction
      if (addr > block.base) {
        // Record the overlap address as a successor so codegen emits a goto
        block.successors.push_back(addr);
        block.has_terminator = true;
        result.blocks.push_back(block);
      }
      block_stack.pop_back();
      continue;
    }

    // CRITICAL: Check if we've hit another function's entry point
    // This enforces the authority system - PDATA/config entries cannot be consumed
    if (addr != entry_point && known_callables_.contains(static_cast<uint32_t>(addr))) {
      REXCODEGEN_TRACE("discover_blocks: hit entry point 0x{:08X} - stopping block", addr);
      if (addr > block.base) {
        // Block has content - save it
        block.end = addr;  // Don't include this instruction
        block.has_terminator = true;
        result.blocks.push_back(block);
      }
      // Either way, pop this block - we can't continue into another function
      block_stack.pop_back();
      continue;
    }

    // Fetch instruction
    auto host_ptr = translate_address<u32>(addr);
    if (!host_ptr) {
      REXCODEGEN_DEBUG("discover_blocks: invalid address 0x{:08X}", addr);
      block.has_terminator = true;
      result.blocks.push_back(block);
      block_stack.pop_back();
      continue;
    }

    u32 code = load_and_swap<u32>(host_ptr);

    // Null instruction ends block
    if (code == 0x00000000) {
      block.end = addr;  // Don't include the null
      block.has_terminator = true;
      result.blocks.push_back(block);
      block_stack.pop_back();
      continue;
    }

    // Include this instruction in block
    block.end = addr + 4;
    scannedAddrs.insert(addr);  // Mark this address as scanned

    Instruction instr = decode_instruction(addr, code);

    // Check for blr (return)
    if (instr.is_return()) {
      block.has_terminator = true;
      result.blocks.push_back(block);
      block_stack.pop_back();
      continue;
    }

    // Check for bctr (indirect branch)
    if (instr.opcode == Opcode::bcctr) {
      auto jt_info = detect_jump_table(addr);
      if (jt_info.has_value()) {
        result.jump_tables.push_back(jt_info.value());
        // Add all jump table targets as successors
        for (guest_addr_t target : jt_info->targets) {
          block.successors.push_back(target);
        }
      }
      block.has_terminator = true;
      result.blocks.push_back(block);
      block_stack.pop_back();

      // Push jump table targets onto stack (if any)
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

    // Check for unconditional branch (b/ba)
    if (instr.opcode == Opcode::b || instr.opcode == Opcode::ba) {
      if (instr.branch_target.has_value()) {
        guest_addr_t target = instr.branch_target.value();
        block.successors.push_back(target);

        // Check known_callables_ FIRST (gathered before discovery)
        bool is_tail_call = known_callables_.contains(static_cast<uint32_t>(target));

        // Backward branch to unknown = probably tail call
        if (!is_tail_call && target < entry_point) {
          is_tail_call = true;
        }

        // Large forward branch (>1MB) = probably tail call to shared code
        // No legitimate internal branch spans more than 1MB
        if (!is_tail_call && target > addr && (target - addr) > 0x100000) {
          is_tail_call = true;
        }

        // Check if target is a known callable (function or import)
        if (!is_tail_call && isKnownCallable(static_cast<uint32_t>(target))) {
          is_tail_call = true;
        }

        // CRITICAL: Check code region boundary
        // If target is in a different region (and not a configured chunk),
        // it MUST be a tail call - prevents mega-merges across null boundaries
        if (!is_tail_call &&
            !isInternalBranch(static_cast<uint32_t>(addr), static_cast<uint32_t>(target),
                              static_cast<uint32_t>(entry_point))) {
          is_tail_call = true;
        }

        // CRITICAL: Check if target looks like a function entry (has prologue)
        // If branching to a prologue, it's definitely a tail call to another function
        if (!is_tail_call && is_prologue_pattern(target)) {
          REXCODEGEN_TRACE("discover_blocks: target 0x{:08X} has prologue pattern (TAIL CALL)",
                           target);
          is_tail_call = true;
        }

        if (is_tail_call) {
          REXCODEGEN_TRACE("discover_blocks: b 0x{:08X} -> 0x{:08X} is TAIL CALL", addr, target);
          result.tail_calls.push_back(target);
        } else if (!scannedAddrs.count(target)) {
          REXCODEGEN_TRACE(
              "discover_blocks: b 0x{:08X} -> 0x{:08X} treated as INTERNAL (entry=0x{:08X})", addr,
              target, entry_point);
          // Carry projection forward if branch is continuous
          bool is_continuous = (target == block.end);
          int64_t carry_projection = -1;
          if (is_continuous && block.projectedSize != -1) {
            carry_projection = block.projectedSize - static_cast<int64_t>(block.end - block.base);
            if (carry_projection <= 0)
              carry_projection = -1;
          }

          // IMPORTANT: Save and pop current block FIRST, before push_back
          // push_back can reallocate the vector, invalidating 'block' reference
          block.has_terminator = true;
          result.blocks.push_back(block);
          block_stack.pop_back();

          // Now safe to push new block
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

    // Check for function call (bl) - doesn't end block
    if (instr.is_call() && instr.branch_target.has_value()) {
      result.external_calls.push_back(instr.branch_target.value());
      // Continue to next instruction (block.end already updated)
      continue;
    }

    // Check for conditional return (bclr/bclrl with conditional BO)
    // These return to LR if condition is met, otherwise fall through
    // Examples: blelr, bgtlr, bnelr, beqlr, etc.
    if ((instr.opcode == Opcode::bclr || instr.opcode == Opcode::bclrl) &&
        !instr.is_return()) {  // is_return() only matches unconditional blr
      guest_addr_t fall_through = addr + 4;
      block.successors.push_back(fall_through);
      block.has_terminator = true;
      result.blocks.push_back(block);
      block_stack.pop_back();  // Pop current block BEFORE pushing fall-through

      // Push fall-through block onto stack
      if (!scannedAddrs.count(fall_through)) {
        DiscoveredBlock ft_block;
        ft_block.base = fall_through;
        ft_block.end = fall_through;
        ft_block.projectedSize = -1;
        block_stack.push_back(ft_block);
      }
      continue;
    }

    // Check for conditional branch (bc, bca, etc.)
    if (instr.is_branch() && instr.branch_target.has_value()) {
      guest_addr_t target = instr.branch_target.value();
      guest_addr_t fall_through = addr + 4;

      // Block ends at conditional branch
      block.successors.push_back(fall_through);
      block.successors.push_back(target);
      result.blocks.push_back(block);
      block_stack.pop_back();

      // Push true-case first, then false-case
      // False-case (fall-through) gets projectedSize = distance to true-case
      // This prevents fall-through from growing past the branch target

      // Check if target is internal to function (at or after entry point)
      bool target_is_internal = (target >= entry_point);

      // Push true-case block (branch target) - no projection
      if (target_is_internal && !scannedAddrs.count(target)) {
        DiscoveredBlock true_block;
        true_block.base = target;
        true_block.end = target;
        true_block.projectedSize = -1;  // No limit on true-case
        block_stack.push_back(true_block);
      }

      // Push false-case block (fall-through) WITH projection
      if (!scannedAddrs.count(fall_through)) {
        DiscoveredBlock false_block;
        false_block.base = fall_through;
        false_block.end = fall_through;

        // Project size: distance from fall-through to branch target
        // This prevents fall-through from consuming code past the branch target
        if (target_is_internal && target > fall_through) {
          false_block.projectedSize = static_cast<int64_t>(target - fall_through);
          REXCODEGEN_TRACE(
              "Conditional branch at 0x{:08X}: fall-through 0x{:08X} projected to 0x{:X} bytes",
              addr, fall_through, false_block.projectedSize);
        } else {
          false_block.projectedSize = -1;
        }
        block_stack.push_back(false_block);
      }
      continue;
    }

    // Regular instruction - continue to next (block.end already updated)
  }

  if (result.blocks.empty()) {
    REXCODEGEN_WARN("discover_blocks: no blocks found for entry 0x{:08X}", entry_point);
  }

  // Sort blocks by address for deterministic output and easier diffing
  std::sort(result.blocks.begin(), result.blocks.end(),
            [](const DiscoveredBlock& a, const DiscoveredBlock& b) { return a.base < b.base; });

  return result;
}

//=============================================================================
// Code Region Boundary Checking
//=============================================================================

const CodeRegion* FunctionScanner::findRegionContaining(uint32_t address) const {
  if (!code_regions_)
    return nullptr;

  for (const auto& region : *code_regions_) {
    if (region.contains(address)) {
      return &region;
    }
  }
  return nullptr;
}

bool FunctionScanner::isInternalBranch(uint32_t currentAddr, uint32_t targetAddr,
                                       uint32_t functionEntry) const {
  // Check if target is a configured chunk of the current function
  // Chunks can cross region boundaries by design
  if (isWithinChunk(targetAddr, functionEntry)) {
    return true;
  }

  // Find regions for both addresses
  const auto* currentRegion = findRegionContaining(currentAddr);
  const auto* targetRegion = findRegionContaining(targetAddr);

  // If target is in a different region (or no region), it's a tail call
  if (currentRegion != targetRegion) {
    REXCODEGEN_TRACE("isInternalBranch: 0x{:08X} -> 0x{:08X} crosses region boundary (TAIL CALL)",
                     currentAddr, targetAddr);
    return false;
  }

  return true;
}

//=============================================================================
// Block Discovery
//=============================================================================

namespace {

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

    // Stop at unconditional terminators - scanning past basic block boundaries
    // risks finding unrelated comparisons on the index register
    if (isTerminator(*insn) && !isConditional(*insn))
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

    // PPC instructions must be 4-byte aligned
    if (target & 3) {
      REXCODEGEN_TRACE(
          "detectJumpTable: bctr=0x{:08X} entry[{}] target=0x{:08X} not 4-byte aligned", bctrAddr,
          i, target);
      if (jt.targets.empty())
        return std::nullopt;
      break;
    }

    // Validate target is within code region - jump table targets help DEFINE function extent
    // Don't constrain by funcEnd since that's just PDATA which may not include out-of-line code
    if (target == 0 || !containingRegion.contains(target)) {
      // TODO(tomc): Figure out what this voodoo does on real hardware. Its a jump target that
      // points to a null value..?
      if (target != 0) {
        auto outsideInsn = decoded.read<uint32_t>(target);
        if (outsideInsn && (*outsideInsn == 0x00000000 || *outsideInsn == 0xFFFFFFFF)) {
          REXCODEGEN_TRACE(
              "detectJumpTable: bctr=0x{:08X} entry[{}] target=0x{:08X} null jump sentinel",
              bctrAddr, i, target);
          jt.targets.push_back(0);  // sentinel
          continue;
        }
      }
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
