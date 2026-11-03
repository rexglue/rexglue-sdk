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

/** The PowerPC instruction formats the decoder needs to find a branch target. */
enum class InstrFormat : u8 {
  kUnknown = 0,
  kI,  /**< Branch (LI, AA, LK). */
  kB,  /**< Conditional branch (BO, BI, BD, AA, LK). */
  kXL, /**< Branch to LR/CTR (BO, BI, XO, LK). */
};

/** The instructions function discovery names, plus two catch all values. */
enum class Opcode : u8 {
  kUnknown = 0, /**< The word decodes to no valid instruction. */
  kOther,       /**< A valid instruction that discovery does not name. */

  b,
  ba,
  bl,
  bla,
  bc,
  bca,
  bcl,
  bcla,
  bclr,
  bclrl,
  bcctr,
  bcctrl,
  sc,
  tw,
  twi,

  lbz,
  lbzx,
  lhz,
  lhzx,
  lwz,
  lwzx,
  stwu,
  add,
  addi,
  addis,
  subf,
  ori,
  and_,
  or_,
  xor_,
  slw,
  srw,
  sraw,
  srawi,
  rlwinm,
  cmpi,
  cmpli,
  li,  /**< addi with RA of 0. */
  lis, /**< addis with RA of 0. */
  mr,
  nop,
  mfspr,
  mtspr,
  mflr,
  mtlr,
  mfctr,
  mtctr,
};

/**
 * Reports whether an instruction ends a block.
 *
 * @return True when the instruction is a branch, sc, tw or twi.
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

/** Reports whether the Xenon CPU lacks an instruction the binutils table still lists. */
bool IsInvalidOnXenon(int id);

/**
 * Returns the PPC_INST ID of a decoded instruction, or 0 when the instruction is not valid on the
 * Xenon CPU. The SH forms of vupkhsb128 and vupklsb128 get the halfword IDs.
 */
int ResolveID(const ppc_insn& insn);

/**
 * Returns the opcode for a matched binutils table entry. The opcode is kOther for a valid
 * instruction discovery does not name and kUnknown when the word is not valid on the Xenon CPU.
 *
 * @param entry The matched entry, or null when the word matches nothing.
 */
Opcode OpcodeOf(const powerpc_opcode* entry);

/**
 * Looks up the ID of an instruction word through the binutils table.
 *
 * @return The PPC_INST ID, or 0 when the word is not valid on the Xenon CPU.
 */
int LookupID(u32 code);

/**
 * Looks up the opcode of an instruction word through the binutils table.
 *
 * @return The opcode, with kUnknown for a word that is not valid on the Xenon CPU.
 */
Opcode LookupOpcode(u32 code);

}  // namespace rex::codegen::detail
