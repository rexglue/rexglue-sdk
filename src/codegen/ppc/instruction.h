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

namespace rex::codegen::detail {

/** A PowerPC instruction with decoded fields. */
struct Instruction {
  u32 address = 0;
  be_u32 code{0};
  Opcode opcode = Opcode::kUnknown;
  InstrFormat format = InstrFormat::kUnknown;

  /** Format I, unconditional branch (b, ba, bl, bla). */
  struct FormatI {
    u32 LI : 24; /**< Branch offset (bits 6-29). */
    u32 AA : 1;  /**< Absolute address flag (bit 30). */
    u32 LK : 1;  /**< Link flag (bit 31). */
    u32 primary_opcode : 6;

    i32 GetOffset() const {
      i32 val = static_cast<i32>(LI << 8) >> 6;
      return val;
    }
  };

  /** Format B, conditional branch (bc, bca, bcl, bcla). */
  struct FormatB {
    u32 BD : 14; /**< Branch displacement (bits 16-29). */
    u32 AA : 1;  /**< Absolute address flag (bit 30). */
    u32 LK : 1;  /**< Link flag (bit 31). */
    u32 BI : 5;  /**< Condition register bit (bits 11-15). */
    u32 BO : 5;  /**< Branch options (bits 6-10). */
    u32 primary_opcode : 6;

    i32 GetOffset() const {
      i32 val = static_cast<i32>(BD << 18) >> 16;
      return val;
    }
  };

  /** Format D, immediate operations (load, store, addi and others). */
  struct FormatD {
    u32 d : 16; /**< Immediate value (bits 16-31). */
    u32 RA : 5; /**< Register A (bits 11-15). */
    u32 RT : 5; /**< Register T/S (bits 6-10). */
    u32 primary_opcode : 6;

    i32 SIMM() const { return static_cast<i32>(static_cast<i16>(d)); }

    u32 UIMM() const { return static_cast<u32>(d); }

    u32 RS() const { return RT; }
  };

  /** Format DS, double word operations (ld, std). */
  struct FormatDS {
    u32 XO : 2;  /**< Extended opcode (bits 30-31). */
    u32 DS : 14; /**< Displacement (bits 16-29, multiple of 4). */
    u32 RA : 5;  /**< Register A (bits 11-15). */
    u32 RT : 5;  /**< Register T/S (bits 6-10). */
    u32 primary_opcode : 6;

    i32 GetDisplacement() const { return static_cast<i32>(static_cast<i16>(DS << 2)); }

    u32 RS() const { return RT; }
  };

  /** Format X, general register operations (logical, shifts, loads and stores). */
  struct FormatX {
    u32 Rc : 1;  /**< Updates CR0. */
    u32 XO : 10; /**< Extended opcode (bits 21-30). */
    u32 RB : 5;  /**< Register B (bits 16-20). */
    u32 RA : 5;  /**< Register A (bits 11-15). */
    u32 RT : 5;  /**< Register T/S (bits 6-10). */
    u32 primary_opcode : 6;

    u32 RS() const { return RT; }
  };

  /** Format XL, branch to LR or CTR (bclr, bcctr). */
  struct FormatXL {
    u32 LK : 1;  /**< Link bit (bit 31). */
    u32 XO : 10; /**< Extended opcode (bits 21-30). */
    u32 unused : 5;
    u32 BI : 5; /**< Condition register bit (bits 11-15). */
    u32 BO : 5; /**< Branch options (bits 6-10). */
    u32 primary_opcode : 6;
  };

  /** Format XFX, SPR access (mfspr, mtspr). */
  struct FormatXFX {
    u32 unused : 1;
    u32 XO : 10;  /**< Extended opcode (bits 21-30). */
    u32 SPR : 10; /**< SPR number (bits 11-20, encoded split). */
    u32 RT : 5;   /**< Register T/S (bits 6-10). */
    u32 primary_opcode : 6;

    /**
     * Returns the SPR number, whose two 5 bit halves are swapped in the encoding.
     */
    u32 GetSPRNum() const {
      u32 lower = (SPR >> 5) & 0x1F;
      u32 upper = SPR & 0x1F;
      return (upper << 5) | lower;
    }

    u32 RS() const { return RT; }
  };

  /** Format XO, arithmetic with overflow (add, sub, mul, div). */
  struct FormatXO {
    u32 Rc : 1; /**< Updates CR0. */
    u32 XO : 9; /**< Extended opcode (bits 22-30). */
    u32 OE : 1; /**< Overflow enable (bit 21). */
    u32 RB : 5; /**< Register B (bits 16-20). */
    u32 RA : 5; /**< Register A (bits 11-15). */
    u32 RT : 5; /**< Register T (bits 6-10). */
    u32 primary_opcode : 6;
  };

  /** Format M, rotate and mask (rlwinm, rlwnm). */
  struct FormatM {
    u32 Rc : 1; /**< Updates CR0. */
    u32 ME : 5; /**< Mask end (bits 26-30). */
    u32 MB : 5; /**< Mask begin (bits 21-25). */
    u32 SH : 5; /**< Shift amount (bits 16-20). */
    u32 RA : 5; /**< Register A (bits 11-15). */
    u32 RS : 5; /**< Register S (bits 6-10). */
    u32 primary_opcode : 6;
  };

  /** Format MD, rotate double word. */
  struct FormatMD {
    u32 Rc : 1; /**< Updates CR0. */
    u32 XO : 3; /**< Extended opcode (bits 27-29). */
    u32 mb : 6; /**< Mask begin (bits 21-26, includes bit 5). */
    u32 sh : 6; /**< Shift amount (bits 16-20, includes bit 30). */
    u32 RA : 5; /**< Register A (bits 11-15). */
    u32 RS : 5; /**< Register S (bits 6-10). */
    u32 primary_opcode : 6;
  };

  /** Format A, floating point arithmetic (fmadd, fmul and others). */
  struct FormatA {
    u32 Rc : 1;  /**< Updates CR0. */
    u32 XO : 5;  /**< Extended opcode (bits 26-30). */
    u32 FRC : 5; /**< FPR C (bits 21-25). */
    u32 FRB : 5; /**< FPR B (bits 16-20). */
    u32 FRA : 5; /**< FPR A (bits 11-15). */
    u32 FRT : 5; /**< FPR T (target, bits 6-10). */
    u32 primary_opcode : 6;
  };

  /** Format VA, vector with four operands (vperm, vmaddfp and others). */
  struct FormatVA {
    u32 XO : 6;  /**< Extended opcode (bits 26-31). */
    u32 VRC : 5; /**< Vector register C (bits 21-25). */
    u32 VRB : 5; /**< Vector register B (bits 16-20). */
    u32 VRA : 5; /**< Vector register A (bits 11-15). */
    u32 VRT : 5; /**< Vector register T (bits 6-10). */
    u32 primary_opcode : 6;

    u32 VD() const { return VRT; }
  };

  /** Format VX, vector with two or three operands (vaddfp, vand and others). */
  struct FormatVX {
    u32 XO : 11; /**< Extended opcode (bits 21-31). */
    u32 VRB : 5; /**< Vector register B (bits 16-20). */
    u32 VRA : 5; /**< Vector register A (bits 11-15). */
    u32 VRT : 5; /**< Vector register T (bits 6-10). */
    u32 primary_opcode : 6;

    u32 UIMM() const { return VRA; }
    i32 SIMM() const { return static_cast<i32>(static_cast<i8>(VRA << 3) >> 3); }
    u32 VD() const { return VRT; }
  };

  /** Format VXR, vector with the Rc bit (vcmp instructions). */
  struct FormatVXR {
    u32 XO : 10; /**< Extended opcode (bits 22-31). */
    u32 Rc : 1;  /**< Sets CR6. */
    u32 VRB : 5; /**< Vector register B (bits 16-20). */
    u32 VRA : 5; /**< Vector register A (bits 11-15). */
    u32 VRT : 5; /**< Vector register T (bits 6-10). */
    u32 primary_opcode : 6;

    u32 VD() const { return VRT; }
  };

  /** Format VMX128, the extended vector format for 128 vector registers. */
  struct FormatVMX128 {
    u32 XO : 6;  /**< Extended opcode (variable position). */
    u32 VRC : 5; /**< Vector register C (bits 21-25), extended. */
    u32 VRB : 5; /**< Vector register B (bits 16-20), extended. */
    u32 VRA : 5; /**< Vector register A (bits 11-15), extended. */
    u32 VRT : 5; /**< Vector register T (bits 6-10), extended. */
    u32 primary_opcode : 6;

    u32 VD() const { return VRT; }
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
    u32 raw;
  };

  /**
   * Returns the I form branch offset for b, ba, bl and bla, which is LI * 4 sign extended from 26
   * bits.
   */
  static i32 GetIOffset(u32 instr) {
    return static_cast<i32>(((instr & 0x3FFFFFC) ^ 0x2000000) - 0x2000000);
  }

  /**
   * Returns the B form branch offset for bc, bca, bcl and bcla, which is BD * 4 sign extended from
   * 16 bits.
   */
  static i32 GetBOffset(u32 instr) {
    return static_cast<i32>(((instr & 0xFFFC) ^ 0x8000) - 0x8000);
  }

  /** Precomputed branch target, empty for indirect branches. */
  std::optional<u32> branch_target;

  bool IsBranch() const;

  /** Reports whether this is a branch with link. */
  bool IsCall() const;

  /** Reports whether this is blr. */
  bool IsReturn() const;

  /** Reports whether this is a conditional branch. Unconditional branches have BO=20. */
  bool IsConditional() const;

  /** Reports whether this is tw or twi with a TO field of 31. */
  bool IsTrapAlways() const;

  /**
   * Reports whether a branch is always taken, meaning b, ba and the bc, bca, bclr and bcctr forms
   * whose BO field ignores both the condition and the count.
   */
  bool IsBranchAlways() const;
};

/** The general purpose and floating point registers an instruction reads and writes. */
struct RegisterEffects {
  u32 gpr_use = 0; /**< Bit n is set when the instruction reads rn. */
  u32 gpr_def = 0; /**< Bit n is set when the instruction writes rn. */
  u32 fpr_use = 0; /**< Bit n is set when the instruction reads fn. */
  u32 fpr_def = 0; /**< Bit n is set when the instruction writes fn. */
};

/**
 * Decodes an instruction from a raw code word.
 *
 * @param code Raw instruction word in host byte order.
 */
Instruction DecodeInstruction(u32 address, u32 code);

/**
 * Returns the general purpose and floating point registers an instruction reads and writes, empty
 * for an undecodable word.
 */
RegisterEffects GetRegisterEffects(const Instruction& instr);

/**
 * Reports whether an instruction allocates the stack frame.
 *
 * @return True for stwu and stdu with r1 as the base.
 */
bool IsFrameAllocation(const Instruction& instr);

/**
 * Reports whether an instruction stores to the caller's frame.
 *
 * @return True for a store to a positive displacement from r1.
 */
bool IsCallerFrameSpill(const Instruction& instr);

}  // namespace rex::codegen::detail
