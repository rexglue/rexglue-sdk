/**
 * @file        codegen/ppc/instruction.h
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
 * Uses union-based decoding for field access.
 */
struct Instruction {
  uint32_t address = 0;
  be_u32 code{0};
  Opcode opcode = Opcode::kUnknown;
  InstrFormat format = InstrFormat::kUnknown;

  /**
   * Format I - Unconditional Branch (b, ba, bl, bla)
   * Fields LI (24-bit), AA (1-bit), LK (1-bit)
   */
  struct FormatI {
    uint32_t LI : 24;  /**< Branch offset (bits 6-29). */
    uint32_t AA : 1;   /**< Absolute address flag (bit 30). */
    uint32_t LK : 1;   /**< Link flag (bit 31). */
    uint32_t _pad : 6; /**< Primary opcode (bits 0-5). */

    int32_t offset() const {
      int32_t val = static_cast<int32_t>(LI << 8) >> 6;
      return val;
    }
  };

  /**
   * Format B - Conditional Branch (bc, bca, bcl, bcla)
   * Fields BO (5-bit), BI (5-bit), BD (14-bit), AA (1-bit), LK (1-bit)
   */
  struct FormatB {
    uint32_t BD : 14;  /**< Branch displacement (bits 16-29). */
    uint32_t AA : 1;   /**< Absolute address flag (bit 30). */
    uint32_t LK : 1;   /**< Link flag (bit 31). */
    uint32_t BI : 5;   /**< Condition register bit (bits 11-15). */
    uint32_t BO : 5;   /**< Branch options (bits 6-10). */
    uint32_t _pad : 6; /**< Primary opcode (bits 0-5). */

    int32_t offset() const {
      int32_t val = static_cast<int32_t>(BD << 18) >> 16;
      return val;
    }
  };

  /**
   * Format D - Immediate operations (load, store, addi and others)
   * Fields RT/RS (5-bit), RA (5-bit), d/SIMM/UIMM (16-bit)
   */
  struct FormatD {
    uint32_t d : 16;   /**< Immediate value (bits 16-31). */
    uint32_t RA : 5;   /**< Register A (bits 11-15). */
    uint32_t RT : 5;   /**< Register T/S (bits 6-10). */
    uint32_t _pad : 6; /**< Primary opcode (bits 0-5). */

    int32_t SIMM() const { return static_cast<int32_t>(static_cast<int16_t>(d)); }

    uint32_t UIMM() const { return static_cast<uint32_t>(d); }

    uint32_t RS() const { return RT; }
  };

  /**
   * Format DS - Double-word operations (ld, std)
   * Fields RT/RS (5-bit), RA (5-bit), DS (14-bit), XO (2-bit)
   */
  struct FormatDS {
    uint32_t XO : 2;   /**< Extended opcode (bits 30-31). */
    uint32_t DS : 14;  /**< Displacement (bits 16-29, multiple of 4). */
    uint32_t RA : 5;   /**< Register A (bits 11-15). */
    uint32_t RT : 5;   /**< Register T/S (bits 6-10). */
    uint32_t _pad : 6; /**< Primary opcode (bits 0-5). */

    int32_t displacement() const { return static_cast<int32_t>(static_cast<int16_t>(DS << 2)); }

    uint32_t RS() const { return RT; }
  };

  /**
   * Format X - General register operations (logical, shifts, loads/stores)
   * Fields RT/RS (5-bit), RA (5-bit), RB (5-bit), XO (10-bit), Rc (1-bit)
   */
  struct FormatX {
    uint32_t Rc : 1;   /**< Bit 31, updates CR0. */
    uint32_t XO : 10;  /**< Extended opcode (bits 21-30). */
    uint32_t RB : 5;   /**< Register B (bits 16-20). */
    uint32_t RA : 5;   /**< Register A (bits 11-15). */
    uint32_t RT : 5;   /**< Register T/S (bits 6-10). */
    uint32_t _pad : 6; /**< Primary opcode (bits 0-5). */

    uint32_t RS() const { return RT; }
  };

  /**
   * Format XL - Branch to LR/CTR (bclr, bcctr)
   * Fields BO (5-bit), BI (5-bit), XO (10-bit), LK (1-bit)
   */
  struct FormatXL {
    uint32_t LK : 1;      /**< Link bit (bit 31). */
    uint32_t XO : 10;     /**< Extended opcode (bits 21-30). */
    uint32_t _unused : 5; /**< Unused (bits 16-20). */
    uint32_t BI : 5;      /**< Condition register bit (bits 11-15). */
    uint32_t BO : 5;      /**< Branch options (bits 6-10). */
    uint32_t _pad : 6;    /**< Primary opcode (bits 0-5). */
  };

  /**
   * Format XFX - SPR access (mfspr, mtspr)
   * Fields RT/RS (5-bit), SPR (10-bit), XO (10-bit)
   */
  struct FormatXFX {
    uint32_t _unused : 1; /**< Unused (bit 31). */
    uint32_t XO : 10;     /**< Extended opcode (bits 21-30). */
    uint32_t SPR : 10;    /**< SPR number (bits 11-20, encoded split). */
    uint32_t RT : 5;      /**< Register T/S (bits 6-10). */
    uint32_t _pad : 6;    /**< Primary opcode (bits 0-5). */

    /**
     * Returns the SPR number, whose 2 5-bit halves are swapped in the encoding.
     *
     * @return The SPR number
     */
    uint32_t spr_num() const {
      uint32_t lower = (SPR >> 5) & 0x1F;
      uint32_t upper = SPR & 0x1F;
      return (upper << 5) | lower;
    }

    uint32_t RS() const { return RT; }
  };

  /**
   * Format XO - Arithmetic with overflow (add, sub, mul, div)
   * Fields RT (5-bit), RA (5-bit), RB (5-bit), OE (1-bit), XO (9-bit), Rc (1-bit)
   */
  struct FormatXO {
    uint32_t Rc : 1;   /**< Bit 31, updates CR0. */
    uint32_t XO : 9;   /**< Extended opcode (bits 22-30). */
    uint32_t OE : 1;   /**< Overflow enable (bit 21). */
    uint32_t RB : 5;   /**< Register B (bits 16-20). */
    uint32_t RA : 5;   /**< Register A (bits 11-15). */
    uint32_t RT : 5;   /**< Register T (bits 6-10). */
    uint32_t _pad : 6; /**< Primary opcode (bits 0-5). */
  };

  /**
   * Format M - Rotate and mask (rlwinm, rlwnm)
   * Fields RS (5-bit), RA (5-bit), SH/RB (5-bit), MB (5-bit), ME (5-bit), Rc (1-bit)
   */
  struct FormatM {
    uint32_t Rc : 1;   /**< Bit 31, updates CR0. */
    uint32_t ME : 5;   /**< Mask end (bits 26-30). */
    uint32_t MB : 5;   /**< Mask begin (bits 21-25). */
    uint32_t SH : 5;   /**< Shift amount (bits 16-20). */
    uint32_t RA : 5;   /**< Register A (bits 11-15). */
    uint32_t RS : 5;   /**< Register S (bits 6-10). */
    uint32_t _pad : 6; /**< Primary opcode (bits 0-5). */

    /** Returns the register B field of rlwnm, which shares the SH bits. */
    uint32_t RB() const { return SH; }
  };

  /**
   * Format MD - Rotate double-word (64-bit)
   * Fields RS (5-bit), RA (5-bit), sh (6-bit), mb (6-bit), XO (3-bit), Rc (1-bit)
   */
  struct FormatMD {
    uint32_t Rc : 1;   /**< Bit 31, updates CR0. */
    uint32_t XO : 3;   /**< Extended opcode (bits 27-30, includes sh bit 5). */
    uint32_t mb : 6;   /**< Mask begin (bits 21-26, includes bit 5). */
    uint32_t sh : 6;   /**< Shift amount (bits 16-20, includes bit 30). */
    uint32_t RA : 5;   /**< Register A (bits 11-15). */
    uint32_t RS : 5;   /**< Register S (bits 6-10). */
    uint32_t _pad : 6; /**< Primary opcode (bits 0-5). */
  };

  /**
   * Format A - Floating-point arithmetic (fmadd, fmul and others)
   * Fields FRT (5-bit), FRA (5-bit), FRB (5-bit), FRC (5-bit), XO (5-bit), Rc (1-bit)
   */
  struct FormatA {
    uint32_t Rc : 1;   /**< Bit 31, updates CR0. */
    uint32_t XO : 5;   /**< Extended opcode (bits 26-30). */
    uint32_t FRC : 5;  /**< FPR C (bits 21-25). */
    uint32_t FRB : 5;  /**< FPR B (bits 16-20). */
    uint32_t FRA : 5;  /**< FPR A (bits 11-15). */
    uint32_t FRT : 5;  /**< FPR T (target, bits 6-10). */
    uint32_t _pad : 6; /**< Primary opcode (bits 0-5). */
  };

  /**
   * Format VA - Vector 4-operand (vperm, vmaddfp and others)
   * Fields VRT (5-bit), VRA (5-bit), VRB (5-bit), VRC (5-bit), XO (6-bit)
   */
  struct FormatVA {
    uint32_t XO : 6;   /**< Extended opcode (bits 26-31). */
    uint32_t VRC : 5;  /**< Vector register C (bits 21-25). */
    uint32_t VRB : 5;  /**< Vector register B (bits 16-20). */
    uint32_t VRA : 5;  /**< Vector register A (bits 11-15). */
    uint32_t VRT : 5;  /**< Vector register T (bits 6-10). */
    uint32_t _pad : 6; /**< Primary opcode (bits 0-5). */

    uint32_t VD() const { return VRT; }
  };

  /**
   * Format VX - Vector 3-operand/2-operand (vaddfp, vand and others)
   * Fields VRT (5-bit), VRA (5-bit), VRB (5-bit), XO (11-bit)
   */
  struct FormatVX {
    uint32_t XO : 11;  /**< Extended opcode (bits 21-31). */
    uint32_t VRB : 5;  /**< Vector register B (bits 16-20). */
    uint32_t VRA : 5;  /**< Vector register A (bits 11-15). */
    uint32_t VRT : 5;  /**< Vector register T (bits 6-10). */
    uint32_t _pad : 6; /**< Primary opcode (bits 0-5). */

    uint32_t UIMM() const { return VRA; }
    int32_t SIMM() const { return static_cast<int32_t>(static_cast<int8_t>(VRA << 3) >> 3); }
    uint32_t VD() const { return VRT; }
  };

  /**
   * Format VXR, vector with the Rc bit (vcmp instructions).
   * Fields VRT (5-bit), VRA (5-bit), VRB (5-bit), Rc (1-bit), XO (10-bit)
   */
  struct FormatVXR {
    uint32_t XO : 10;  /**< Extended opcode (bits 21-30). */
    uint32_t Rc : 1;   /**< Bit 31, sets CR6. */
    uint32_t VRB : 5;  /**< Vector register B (bits 16-20). */
    uint32_t VRA : 5;  /**< Vector register A (bits 11-15). */
    uint32_t VRT : 5;  /**< Vector register T (bits 6-10). */
    uint32_t _pad : 6; /**< Primary opcode (bits 0-5). */

    uint32_t VD() const { return VRT; }
  };

  /**
   * Format VMX128 - Console extended vector format
   * Uses different register encoding for 128 vector registers
   */
  struct FormatVMX128 {
    uint32_t XO : 6;   /**< Extended opcode (variable position). */
    uint32_t VRC : 5;  /**< Vector register C (bits 21-25), extended. */
    uint32_t VRB : 5;  /**< Vector register B (bits 16-20), extended. */
    uint32_t VRA : 5;  /**< Vector register A (bits 11-15), extended. */
    uint32_t VRT : 5;  /**< Vector register T (bits 6-10), extended. */
    uint32_t _pad : 6; /**< Primary opcode (bits 0-5). */

    uint32_t VD() const { return VRT; }
  };

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
    FormatA A;
    FormatVA VA;
    FormatVX VX;
    FormatVXR VXR;
    FormatVMX128 VMX128;
    uint32_t raw;
  };

  /**
   * Returns the I-form branch offset for b, ba, bl and bla.
   *
   * LI is bits 6-29 with an implicit 00 suffix, so mask 0x3FFFFFC (bits 2-25) yields LI * 4. The
   * XOR-subtract with sign bit 0x2000000 sign extends from 26 bits.
   *
   * @param instr Raw instruction word
   * @return Sign extended byte offset
   */
  static int32_t get_i_offset(uint32_t instr) {
    return static_cast<int32_t>(((instr & 0x3FFFFFC) ^ 0x2000000) - 0x2000000);
  }

  /**
   * Returns the B-form branch offset for bc, bca, bcl and bcla.
   *
   * BD is bits 16-29 with an implicit 00 suffix, so mask 0xFFFC (bits 2-15) yields BD * 4. The
   * XOR-subtract with sign bit 0x8000 sign extends from 16 bits.
   *
   * @param instr Raw instruction word
   * @return Sign extended byte offset
   */
  static int32_t get_b_offset(uint32_t instr) {
    return static_cast<int32_t>(((instr & 0xFFFC) ^ 0x8000) - 0x8000);
  }

  /** Pre-computed branch target, empty for indirect branches. */
  std::optional<uint32_t> branch_target;

  /**
   * Reports whether this is a branch instruction.
   *
   * @return True for a branch
   */
  bool is_branch() const;

  /**
   * Reports whether this is a function call (branch with link).
   *
   * @return True for a call
   */
  bool is_call() const;

  /**
   * Reports whether this is a return instruction (blr).
   *
   * @return True for a return
   */
  bool is_return() const;

  /**
   * Reports whether this is a conditional branch, false for unconditional branches (BO=20).
   *
   * @return True for a conditional branch
   */
  bool is_conditional() const;

  /**
   * Disassembles the instruction to a string.
   *
   * @return The disassembly text
   */
  std::string to_string() const;
};

/**
 * Decode instruction from raw code
 *
 * @param address Guest address of instruction
 * @param code Raw 32-bit instruction code (host byte order)
 * @return Decoded instruction structure
 */
Instruction decode_instruction(uint32_t address, uint32_t code);

}  // namespace rex::codegen::ppc
