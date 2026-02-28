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
#include "ppc/instruction.h"
#include "ppc/opcode.h"

#include <algorithm>
#include <set>
#include <stack>
#include <unordered_set>

#include <fmt/format.h>

#include <rex/codegen/binary_view.h>
#include <rex/codegen/codegen_context.h>
#include <rex/codegen/recompiled_function.h>
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
  u32 entry_count = 0;
  u8 index_register = 0;
  guest_addr_t default_target = 0;
  bool found = false;
};

//=============================================================================
// Helper: Scan for bounds check (CMPLWI + BGT/BLE)
//=============================================================================

BoundsInfo scan_for_bounds(const FunctionScanner& scanner, guest_addr_t bctr_address,
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
            bounds.default_target = instr.branch_target.value();
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
          bounds.entry_count = implicit_count;
          bounds.index_register = expected_index_reg;
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
        bounds.entry_count = cmp_imm + 1;  // cmpli compares against max, so count = max + 1
        bounds.index_register = cmp_ra;
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
  BoundsInfo bounds = scan_for_bounds(*this, bctr_address, match.index_reg);
  REXCODEGEN_TRACE("  Bounds check: found={}, count={}, default=0x{:08X}, index_reg=r{}",
                   bounds.found, bounds.entry_count, bounds.default_target, bounds.index_register);

  // Read table entries
  std::vector<guest_addr_t> targets = read_table_entries(*this, match, bounds.entry_count);

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

}  // namespace rex::codegen
