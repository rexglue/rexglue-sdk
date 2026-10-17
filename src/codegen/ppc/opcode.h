/**
 * @file        codegen/ppc/opcode.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

#include <rex/types.h>

struct ppc_insn;
struct powerpc_opcode;

namespace rex::codegen::detail {

/**
 * PowerPC instruction formats the decoder needs to find a branch target
 */
enum class InstrFormat : u8 {
  kUnknown = 0,
  kI,  /**< Branch (LI, AA, LK). */
  kB,  /**< Conditional branch (BO, BI, BD, AA, LK). */
  kXL, /**< Branch to LR/CTR (BO, BI, XO, LK). */
};

/**
 * The instructions function discovery names, the 2 catch all values and nothing else.
 */
enum class Opcode : u8 {
  kUnknown = 0, /**< The word decodes to no valid instruction. */
  kOther,       /**< A valid instruction that discovery does not name. */

  b,      /**< Branch. */
  ba,     /**< Branch absolute. */
  bl,     /**< Branch and link. */
  bla,    /**< Branch and link absolute. */
  bc,     /**< Branch conditional. */
  bca,    /**< Branch conditional absolute. */
  bcl,    /**< Branch conditional and link. */
  bcla,   /**< Branch conditional and link absolute. */
  bclr,   /**< Branch conditional to link register. */
  bclrl,  /**< Branch conditional to link register and link. */
  bcctr,  /**< Branch conditional to count register. */
  bcctrl, /**< Branch conditional to count register and link. */
  sc,     /**< System call. */
  tw,     /**< Trap word. */
  twi,    /**< Trap word immediate. */

  lbz,    /**< Load byte and zero. */
  lbzx,   /**< Load byte and zero indexed. */
  lhz,    /**< Load halfword and zero. */
  lhzx,   /**< Load halfword and zero indexed. */
  lwz,    /**< Load word and zero. */
  lwzx,   /**< Load word and zero indexed. */
  stwu,   /**< Store word with update. */
  add,    /**< Add. */
  addi,   /**< Add immediate. */
  addis,  /**< Add immediate shifted. */
  subf,   /**< Subtract from. */
  ori,    /**< OR immediate. */
  and_,   /**< AND, written and in assembly. */
  or_,    /**< OR. */
  xor_,   /**< XOR. */
  slw,    /**< Shift left word. */
  srw,    /**< Shift right word. */
  sraw,   /**< Shift right algebraic word. */
  srawi,  /**< Shift right algebraic word immediate. */
  rlwinm, /**< Rotate left word immediate then AND with mask. */
  cmpi,   /**< Compare immediate. */
  cmpli,  /**< Compare logical immediate. */
  li,     /**< Load immediate, addi with RA of 0. */
  lis,    /**< Load immediate shifted, addis with RA of 0. */
  mr,     /**< Move register. */
  nop,    /**< No operation. */
  mfspr,  /**< Move from special purpose register. */
  mtspr,  /**< Move to special purpose register. */
  mflr,   /**< Move from link register. */
  mtlr,   /**< Move to link register. */
  mfctr,  /**< Move from count register. */
  mtctr,  /**< Move to count register. */
};

/**
 * Reports whether an instruction ends a block.
 *
 * @param op The instruction.
 * @return True for every branch, sc, tw and twi.
 */
inline bool IsTerminatorInstruction(Opcode op) {
  switch (op) {
    case Opcode::b:
    case Opcode::ba:
    case Opcode::bl:
    case Opcode::bla:
    case Opcode::bc:
    case Opcode::bca:
    case Opcode::bcl:
    case Opcode::bcla:
    case Opcode::bclr:
    case Opcode::bclrl:
    case Opcode::bcctr:
    case Opcode::bcctrl:
    case Opcode::sc:
    case Opcode::tw:
    case Opcode::twi:
      return true;
    default:
      return false;
  }
}

/**
 * Reports whether the Xenon CPU dropped an instruction the binutils table still lists.
 *
 * @param id The PPC_INST id.
 * @return True when the instruction is not valid on the Xenon CPU.
 */
bool IsDropped(int id);

/**
 * Gives the id the dispatch and discovery use for a decoded instruction.
 *
 * An instruction the Xenon CPU dropped has no id. The SH forms of vupkhsb128 and vupklsb128 that
 * the binutils table reports as the byte forms get the halfword ids.
 *
 * @param insn The decoded instruction.
 * @return The PPC_INST id, or 0 when the instruction is not valid on the Xenon CPU.
 */
int ResolveID(const ppc_insn& insn);

/**
 * Names a matched binutils table entry for function discovery.
 *
 * @param entry The matched entry, or null when the word matches nothing.
 * @return The opcode, kOther for a valid instruction discovery does not name and kUnknown when the
 * word is not valid on the Xenon CPU.
 */
Opcode OpcodeOf(const powerpc_opcode* entry);

/**
 * Looks up the id of an instruction word through the binutils table.
 *
 * @param code Raw 32-bit instruction.
 * @return The PPC_INST id, or 0 when the word is not valid on the Xenon CPU.
 */
int LookupID(u32 code);

/**
 * Looks up the opcode of an instruction word through the binutils table.
 *
 * @param code Raw 32-bit instruction.
 * @return The opcode, with kUnknown for a word that is not valid on the Xenon CPU.
 */
Opcode LookupOpcode(u32 code);

}  // namespace rex::codegen::detail
