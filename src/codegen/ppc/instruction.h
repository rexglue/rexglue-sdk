/**
 * @file        arch/ppc/instruction.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "opcode.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rex/types.h>

namespace rex::codegen::ppc {

/**
 * PowerPC instruction with decoded fields
 *
 * Uses union-based decoding for efficient field access.
 * Includes semantic information for future recompilation support.
 */
struct Instruction {
  uint32_t address = 0;  // Guest address of instruction
  be_u32 code{0};        // Raw instruction encoding (big-endian)
  Opcode opcode = Opcode::kUnknown;
  InstrFormat format = InstrFormat::kUnknown;

  //=========================================================================
  // Format-specific field unions
  //=========================================================================

  /**
   * Format I - Unconditional Branch (b, ba, bl, bla)
   * Fields: LI (24-bit), AA (1-bit), LK (1-bit)
   */
  struct FormatI {
    uint32_t LI : 24;   // Branch offset (bits 6-29)
    uint32_t AA : 1;    // Absolute address flag (bit 30)
    uint32_t LK : 1;    // Link flag (bit 31)
    uint32_t _pad : 6;  // Primary opcode (bits 0-5)

    // Get sign-extended branch target offset
    int32_t offset() const {
      // Sign-extend 24-bit to 32-bit, then multiply by 4
      int32_t val = static_cast<int32_t>(LI << 8) >> 6;  // Sign extend and * 4
      return val;
    }
  };

  /**
   * Format B - Conditional Branch (bc, bca, bcl, bcla)
   * Fields: BO (5-bit), BI (5-bit), BD (14-bit), AA (1-bit), LK (1-bit)
   */
  struct FormatB {
    uint32_t BD : 14;   // Branch displacement (bits 16-29)
    uint32_t AA : 1;    // Absolute address flag (bit 30)
    uint32_t LK : 1;    // Link flag (bit 31)
    uint32_t BI : 5;    // Condition register bit (bits 11-15)
    uint32_t BO : 5;    // Branch options (bits 6-10)
    uint32_t _pad : 6;  // Primary opcode (bits 0-5)

    // Get sign-extended branch target offset
    int32_t offset() const {
      // Sign-extend 14-bit to 32-bit, then multiply by 4
      int32_t val = static_cast<int32_t>(BD << 18) >> 16;  // Sign extend and * 4
      return val;
    }
  };

  /**
   * Format D - Immediate operations (load, store, addi and others)
   * Fields: RT/RS (5-bit), RA (5-bit), d/SIMM/UIMM (16-bit)
   */
  struct FormatD {
    uint32_t d : 16;    // Immediate value (bits 16-31)
    uint32_t RA : 5;    // Register A (bits 11-15)
    uint32_t RT : 5;    // Register T/S (bits 6-10)
    uint32_t _pad : 6;  // Primary opcode (bits 0-5)

    // Signed immediate
    int32_t SIMM() const {
      return static_cast<int32_t>(static_cast<int16_t>(d));  // Sign-extend 16-bit
    }

    // Unsigned immediate
    uint32_t UIMM() const { return static_cast<uint32_t>(d); }

    // Register S (store instructions use this field)
    uint32_t RS() const { return RT; }
  };

  /**
   * Format DS - Double-word operations (ld, std)
   * Fields: RT/RS (5-bit), RA (5-bit), DS (14-bit), XO (2-bit)
   */
  struct FormatDS {
    uint32_t XO : 2;    // Extended opcode (bits 30-31)
    uint32_t DS : 14;   // Displacement (bits 16-29, multiple of 4)
    uint32_t RA : 5;    // Register A (bits 11-15)
    uint32_t RT : 5;    // Register T/S (bits 6-10)
    uint32_t _pad : 6;  // Primary opcode (bits 0-5)

    // Get sign-extended displacement (multiple of 4)
    int32_t displacement() const {
      return static_cast<int32_t>(static_cast<int16_t>(DS << 2));  // Sign-extend and * 4
    }

    uint32_t RS() const { return RT; }
  };

  /**
   * Format X - General register operations (logical, shifts, loads/stores)
   * Fields: RT/RS (5-bit), RA (5-bit), RB (5-bit), XO (10-bit), Rc (1-bit)
   */
  struct FormatX {
    uint32_t Rc : 1;    // Record bit (bit 31)
    uint32_t XO : 10;   // Extended opcode (bits 21-30)
    uint32_t RB : 5;    // Register B (bits 16-20)
    uint32_t RA : 5;    // Register A (bits 11-15)
    uint32_t RT : 5;    // Register T/S (bits 6-10)
    uint32_t _pad : 6;  // Primary opcode (bits 0-5)

    uint32_t RS() const { return RT; }
  };

  /**
   * Format XL - Branch to LR/CTR (bclr, bcctr)
   * Fields: BO (5-bit), BI (5-bit), XO (10-bit), LK (1-bit)
   */
  struct FormatXL {
    uint32_t LK : 1;       // Link bit (bit 31)
    uint32_t XO : 10;      // Extended opcode (bits 21-30)
    uint32_t _unused : 5;  // Unused (bits 16-20)
    uint32_t BI : 5;       // Condition register bit (bits 11-15)
    uint32_t BO : 5;       // Branch options (bits 6-10)
    uint32_t _pad : 6;     // Primary opcode (bits 0-5)
  };

  /**
   * Format XFX - SPR access (mfspr, mtspr)
   * Fields: RT/RS (5-bit), SPR (10-bit), XO (10-bit)
   */
  struct FormatXFX {
    uint32_t _unused : 1;  // Unused (bit 31)
    uint32_t XO : 10;      // Extended opcode (bits 21-30)
    uint32_t SPR : 10;     // SPR number (bits 11-20, encoded split)
    uint32_t RT : 5;       // Register T/S (bits 6-10)
    uint32_t _pad : 6;     // Primary opcode (bits 0-5)

    // Get actual SPR number (bits are swapped in encoding)
    uint32_t spr_num() const {
      uint32_t lower = (SPR >> 5) & 0x1F;
      uint32_t upper = SPR & 0x1F;
      return (upper << 5) | lower;
    }

    uint32_t RS() const { return RT; }
  };

  /**
   * Format XO - Arithmetic with overflow (add, sub, mul, div)
   * Fields: RT (5-bit), RA (5-bit), RB (5-bit), OE (1-bit), XO (9-bit), Rc (1-bit)
   */
  struct FormatXO {
    uint32_t Rc : 1;    // Record bit (bit 31)
    uint32_t XO : 9;    // Extended opcode (bits 22-30)
    uint32_t OE : 1;    // Overflow enable (bit 21)
    uint32_t RB : 5;    // Register B (bits 16-20)
    uint32_t RA : 5;    // Register A (bits 11-15)
    uint32_t RT : 5;    // Register T (bits 6-10)
    uint32_t _pad : 6;  // Primary opcode (bits 0-5)
  };

  /**
   * Format M - Rotate and mask (rlwinm, rlwnm)
   * Fields: RS (5-bit), RA (5-bit), SH/RB (5-bit), MB (5-bit), ME (5-bit), Rc (1-bit)
   */
  struct FormatM {
    uint32_t Rc : 1;    // Record bit (bit 31)
    uint32_t ME : 5;    // Mask end (bits 26-30)
    uint32_t MB : 5;    // Mask begin (bits 21-25)
    uint32_t SH : 5;    // Shift amount (bits 16-20)
    uint32_t RA : 5;    // Register A (bits 11-15)
    uint32_t RS : 5;    // Register S (bits 6-10)
    uint32_t _pad : 6;  // Primary opcode (bits 0-5)

    uint32_t RB() const { return SH; }  // For rlwnm
  };

  /**
   * Format MD - Rotate double-word (64-bit)
   * Fields: RS (5-bit), RA (5-bit), sh (6-bit), mb (6-bit), XO (3-bit), Rc (1-bit)
   */
  struct FormatMD {
    uint32_t Rc : 1;    // Record bit (bit 31)
    uint32_t XO : 3;    // Extended opcode (bits 27-30, includes sh bit 5)
    uint32_t mb : 6;    // Mask begin (bits 21-26, includes bit 5)
    uint32_t sh : 6;    // Shift amount (bits 16-20, includes bit 30)
    uint32_t RA : 5;    // Register A (bits 11-15)
    uint32_t RS : 5;    // Register S (bits 6-10)
    uint32_t _pad : 6;  // Primary opcode (bits 0-5)
  };

  /**
   * Format A - Floating-point arithmetic (fmadd, fmul and others)
   * Fields: FRT (5-bit), FRA (5-bit), FRB (5-bit), FRC (5-bit), XO (5-bit), Rc (1-bit)
   */
  struct FormatA {
    uint32_t Rc : 1;    // Record bit (bit 31)
    uint32_t XO : 5;    // Extended opcode (bits 26-30)
    uint32_t FRC : 5;   // FPR C (bits 21-25)
    uint32_t FRB : 5;   // FPR B (bits 16-20)
    uint32_t FRA : 5;   // FPR A (bits 11-15)
    uint32_t FRT : 5;   // FPR T (target, bits 6-10)
    uint32_t _pad : 6;  // Primary opcode (bits 0-5)
  };

  /**
   * Format VA - Vector 4-operand (vperm, vmaddfp and others)
   * Fields: VRT (5-bit), VRA (5-bit), VRB (5-bit), VRC (5-bit), XO (6-bit)
   */
  struct FormatVA {
    uint32_t XO : 6;    // Extended opcode (bits 26-31)
    uint32_t VRC : 5;   // Vector register C (bits 21-25)
    uint32_t VRB : 5;   // Vector register B (bits 16-20)
    uint32_t VRA : 5;   // Vector register A (bits 11-15)
    uint32_t VRT : 5;   // Vector register T (bits 6-10)
    uint32_t _pad : 6;  // Primary opcode (bits 0-5)

    // Alias for VD (destination)
    uint32_t VD() const { return VRT; }
  };

  /**
   * Format VX - Vector 3-operand/2-operand (vaddfp, vand and others)
   * Fields: VRT (5-bit), VRA (5-bit), VRB (5-bit), XO (11-bit)
   */
  struct FormatVX {
    uint32_t XO : 11;   // Extended opcode (bits 21-31)
    uint32_t VRB : 5;   // Vector register B (bits 16-20)
    uint32_t VRA : 5;   // Vector register A (bits 11-15)
    uint32_t VRT : 5;   // Vector register T (bits 6-10)
    uint32_t _pad : 6;  // Primary opcode (bits 0-5)

    // Alias for immediate in some instructions (UIMM in VRA field)
    uint32_t UIMM() const { return VRA; }
    int32_t SIMM() const {
      return static_cast<int32_t>(static_cast<int8_t>(VRA << 3) >> 3);
    }  // Sign extend 5-bit
    uint32_t VD() const { return VRT; }
  };

  /**
   * Format VXR - Vector with record bit (vcmp instructions)
   * Fields: VRT (5-bit), VRA (5-bit), VRB (5-bit), Rc (1-bit), XO (10-bit)
   */
  struct FormatVXR {
    uint32_t XO : 10;   // Extended opcode (bits 21-30)
    uint32_t Rc : 1;    // Record bit (bit 31, sets CR6)
    uint32_t VRB : 5;   // Vector register B (bits 16-20)
    uint32_t VRA : 5;   // Vector register A (bits 11-15)
    uint32_t VRT : 5;   // Vector register T (bits 6-10)
    uint32_t _pad : 6;  // Primary opcode (bits 0-5)

    uint32_t VD() const { return VRT; }
  };

  /**
   * Format VMX128 - Console extended vector format
   * Uses different register encoding for 128 vector registers
   */
  struct FormatVMX128 {
    uint32_t XO : 6;    // Extended opcode (variable position)
    uint32_t VRC : 5;   // Vector register C (bits 21-25), extended
    uint32_t VRB : 5;   // Vector register B (bits 16-20), extended
    uint32_t VRA : 5;   // Vector register A (bits 11-15), extended
    uint32_t VRT : 5;   // Vector register T (bits 6-10), extended
    uint32_t _pad : 6;  // Primary opcode (bits 0-5)

    // Get full 7-bit register numbers (0-127)
    uint32_t VD() const { return VRT; }
  };

  // Union to access different formats
  union {
    FormatI I;
    FormatB B;
    FormatD D;
    FormatDS DS;
    FormatX X;
    FormatXL XL;
    FormatXFX XFX;
    FormatXO XO;
    FormatM M;
    FormatMD MD;
    FormatA A;            // Floating-point A-form
    FormatVA VA;          // Vector A-form (4 operands)
    FormatVX VX;          // Vector X-form (3 operands)
    FormatVXR VXR;        // Vector X-form with record bit
    FormatVMX128 VMX128;  // VMX128 extended
    uint32_t raw;         // Raw 32-bit value for direct access
  };

  //=========================================================================
  // Branch offset extraction using XOR-subtract sign extension
  //=========================================================================

  /**
   * Get I-form branch offset (26-bit LI field, sign-extended, * 4)
   * For b, ba, bl, bla instructions
   * Uses XOR-subtract technique for reliable sign extension
   */
  static int32_t get_i_offset(uint32_t instr) {
    // LI is bits 6-29 (24 bits), but stored with implicit 00 suffix
    // Mask 0x3FFFFFC extracts bits 2-25 (which is LI * 4)
    // XOR-subtract with sign bit 0x2000000 for 26-bit sign extension
    return static_cast<int32_t>(((instr & 0x3FFFFFC) ^ 0x2000000) - 0x2000000);
  }

  /**
   * Get B-form branch offset (14-bit BD field, sign-extended, * 4)
   * For bc, bca, bcl, bcla instructions
   * Uses XOR-subtract technique for reliable sign extension
   */
  static int32_t get_b_offset(uint32_t instr) {
    // BD is bits 16-29 (14 bits), but stored with implicit 00 suffix
    // Mask 0xFFFC extracts bits 2-15 (which is BD * 4)
    // XOR-subtract with sign bit 0x8000 for 16-bit sign extension
    return static_cast<int32_t>(((instr & 0xFFFC) ^ 0x8000) - 0x8000);
  }

  // Pre-computed branch target (if applicable)
  std::optional<uint32_t> branch_target;

  //=========================================================================
  // Helper methods
  //=========================================================================

  /**
   * Check if this is a branch instruction
   */
  bool is_branch() const;

  /**
   * Check if this is a function call (branch with link)
   */
  bool is_call() const;

  /**
   * Check if this is a return instruction (blr)
   */
  bool is_return() const;

  /**
   * Check if this is a conditional branch
   * Returns false for unconditional branches (BO=20)
   */
  bool is_conditional() const;

  /**
   * Disassemble instruction to string
   */
  std::string to_string() const;
};

/**
 * Decode instruction from raw code
 * @param address Guest address of instruction
 * @param code Raw 32-bit instruction code (host byte order)
 * @return Decoded instruction structure
 */
Instruction decode_instruction(uint32_t address, uint32_t code);

}  // namespace rex::codegen::ppc
