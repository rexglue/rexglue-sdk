/**
 * @file        arch/ppc/opcode.h
 * @brief       PowerPC opcode definitions for Xbox 360
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

namespace rex::codegen::ppc {

/**
 * PowerPC instruction formats
 */
enum class InstrFormat : uint8_t {
  kUnknown = 0,
  kI,    // Branch (LI, AA, LK)
  kB,    // Conditional branch (BO, BI, BD, AA, LK)
  kD,    // Immediate (RT, RA, d/SIMM/UIMM)
  kDS,   // Double-word store (RT, RA, DS, XO)
  kX,    // General purpose (RT, RA, RB, XO, Rc)
  kXL,   // Branch to LR/CTR (BO, BI, XO, LK)
  kXFX,  // Move to/from SPR (RT, SPR, XO)
  kXO,   // Arithmetic with OE (RT, RA, RB, OE, XO, Rc)
  kM,    // Rotate/mask (RS, RA, RB, MB, ME, Rc)
  kMD,   // Rotate double-word (RS, RA, sh, mb, XO, Rc)
  kA,    // Floating-point arithmetic (FRT, FRA, FRB, FRC, XO, Rc)
  kVXR,  // Vector with record bit (VRT, VRA, VRB, Rc, XO)
};

/**
 * PowerPC opcodes
 * Covers essential control flow, ALU, memory, and special register operations
 */
enum class Opcode : uint16_t {
  kUnknown = 0,

  // Branch instructions (essential for control flow)
  b,       // Branch
  ba,      // Branch absolute
  bl,      // Branch and link
  bla,     // Branch and link absolute
  bc,      // Branch conditional
  bca,     // Branch conditional absolute
  bcl,     // Branch conditional and link
  bcla,    // Branch conditional and link absolute
  bclr,    // Branch conditional to link register
  bclrl,   // Branch conditional to link register and link
  bcctr,   // Branch conditional to count register
  bcctrl,  // Branch conditional to count register and link

  // Load instructions (byte, half-word, word, double-word)
  lbz,   // Load byte and zero
  lbzu,  // Load byte and zero with update
  lbzx,  // Load byte and zero indexed
  lhz,   // Load half-word and zero
  lhzu,  // Load half-word and zero with update
  lhzx,  // Load half-word and zero indexed
  lha,   // Load half-word algebraic
  lhax,  // Load half-word algebraic indexed
  lwz,   // Load word and zero
  lwzu,  // Load word and zero with update
  lwzx,  // Load word and zero indexed
  ld,    // Load double-word
  ldu,   // Load double-word with update
  ldx,   // Load double-word indexed

  // Store instructions
  stb,   // Store byte
  stbu,  // Store byte with update
  stbx,  // Store byte indexed
  sth,   // Store half-word
  sthu,  // Store half-word with update
  sthx,  // Store half-word indexed
  stw,   // Store word
  stwu,  // Store word with update
  stwx,  // Store word indexed
  std,   // Store double-word
  stdu,  // Store double-word with update
  stdx,  // Store double-word indexed

  // Integer arithmetic/logical
  add,     // Add
  addi,    // Add immediate
  addis,   // Add immediate shifted
  addic,   // Add immediate carrying
  addic_,  // Add immediate carrying and record
  subf,    // Subtract from
  subfic,  // Subtract from immediate carrying
  neg,     // Negate
  ori,     // OR immediate
  oris,    // OR immediate shifted
  xori,    // XOR immediate
  xoris,   // XOR immediate shifted
  andi_,   // AND immediate (note: andi. in assembly)
  andis_,  // AND immediate shifted
  mulli,   // Multiply low immediate

  // Multiply/divide
  mullw,   // Multiply low word
  mulhw,   // Multiply high word (signed)
  mulhwu,  // Multiply high word (unsigned)
  divw,    // Divide word (signed)
  divwu,   // Divide word (unsigned)

  // Logical operations
  and_,  // AND (note: and in assembly)
  or_,   // OR
  xor_,  // XOR
  nand,  // NAND
  nor,   // NOR
  andc,  // AND with complement
  orc,   // OR with complement
  eqv,   // Equivalent (XNOR)

  // Shifts and rotates
  slw,     // Shift left word
  srw,     // Shift right word
  sraw,    // Shift right algebraic word
  srawi,   // Shift right algebraic word immediate
  rlwinm,  // Rotate left word immediate then AND with mask
  rlwnm,   // Rotate left word then AND with mask
  cntlzw,  // Count leading zeros word

  // Sign/zero extension
  extsb,  // Extend sign byte
  extsh,  // Extend sign halfword

  // Comparison
  cmp,    // Compare
  cmpi,   // Compare immediate
  cmpl,   // Compare logical
  cmpli,  // Compare logical immediate

  // Special purpose register access
  mfspr,  // Move from special purpose register
  mtspr,  // Move to special purpose register
  mfcr,   // Move from condition register
  mtcr,   // Move to condition register

  // Move to/from count/link registers (simplified mnemonics)
  mflr,   // Move from link register (mfspr simplified)
  mtlr,   // Move to link register (mtspr simplified)
  mfctr,  // Move from count register (mfspr simplified)
  mtctr,  // Move to count register (mtspr simplified)

  // Synchronization
  sync,   // Synchronize
  isync,  // Instruction synchronize

  // System call
  sc,  // System call

  // Trap
  tw,   // Trap word
  twi,  // Trap word immediate

  // Move register (simplified)
  mr,  // Move register (or rA, rS, rS)

  // No-op
  nop,  // No operation (ori 0, 0, 0)

  // Load immediate (simplified)
  li,   // Load immediate (addi rD, 0, value)
  lis,  // Load immediate shifted (addis rD, 0, value)

  //=========================================================================
  // Floating-Point Instructions
  //=========================================================================

  // Floating-point load/store
  lfs,    // Load floating-point single
  lfsu,   // Load floating-point single with update
  lfsx,   // Load floating-point single indexed
  lfd,    // Load floating-point double
  lfdu,   // Load floating-point double with update
  lfdx,   // Load floating-point double indexed
  stfs,   // Store floating-point single
  stfsu,  // Store floating-point single with update
  stfsx,  // Store floating-point single indexed
  stfd,   // Store floating-point double
  stfdu,  // Store floating-point double with update
  stfdx,  // Store floating-point double indexed

  // Floating-point arithmetic
  fadd,      // Floating add (double)
  fadds,     // Floating add single
  fsub,      // Floating subtract (double)
  fsubs,     // Floating subtract single
  fmul,      // Floating multiply (double)
  fmuls,     // Floating multiply single
  fdiv,      // Floating divide (double)
  fdivs,     // Floating divide single
  fsqrt,     // Floating square root (double)
  fsqrts,    // Floating square root single
  fre,       // Floating reciprocal estimate
  fres,      // Floating reciprocal estimate single
  frsqrte,   // Floating reciprocal square root estimate
  frsqrtes,  // Floating reciprocal square root estimate single

  // Floating-point multiply-add
  fmadd,    // Floating multiply-add (double)
  fmadds,   // Floating multiply-add single
  fmsub,    // Floating multiply-subtract (double)
  fmsubs,   // Floating multiply-subtract single
  fnmadd,   // Floating negative multiply-add (double)
  fnmadds,  // Floating negative multiply-add single
  fnmsub,   // Floating negative multiply-subtract (double)
  fnmsubs,  // Floating negative multiply-subtract single

  // Floating-point rounding/conversion
  frsp,    // Floating round to single precision
  fctiw,   // Floating convert to integer word
  fctiwz,  // Floating convert to integer word with round toward zero
  fcfid,   // Floating convert from integer doubleword
  fctid,   // Floating convert to integer doubleword
  fctidz,  // Floating convert to integer doubleword with round toward zero

  // Floating-point move/misc
  fmr,    // Floating move register
  fabs,   // Floating absolute value
  fnabs,  // Floating negative absolute value
  fneg,   // Floating negate
  fsel,   // Floating select

  // Floating-point compare
  fcmpu,  // Floating compare unordered
  fcmpo,  // Floating compare ordered

  // Floating-point status/control
  mffs,    // Move from FPSCR
  mtfsf,   // Move to FPSCR fields
  mtfsfi,  // Move to FPSCR field immediate
  mtfsb0,  // Move to FPSCR bit 0
  mtfsb1,  // Move to FPSCR bit 1

  //=========================================================================
  // VMX/VMX128 Vector Instructions (Xbox 360)
  //=========================================================================

  // Vector load/store (standard VMX)
  lvx,    // Load vector indexed
  lvxl,   // Load vector indexed LRU
  stvx,   // Store vector indexed
  stvxl,  // Store vector indexed LRU
  lvlx,   // Load vector left indexed
  lvrx,   // Load vector right indexed
  stvlx,  // Store vector left indexed
  stvrx,  // Store vector right indexed
  lvsl,   // Load vector for shift left
  lvsr,   // Load vector for shift right

  // Vector load/store (VMX128 extended - 128 registers)
  lvx128,     // Load vector indexed (128-reg)
  stvx128,    // Store vector indexed (128-reg)
  lvlx128,    // Load vector left indexed (128-reg)
  lvrx128,    // Load vector right indexed (128-reg)
  stvlx128,   // Store vector left indexed (128-reg)
  stvrx128,   // Store vector right indexed (128-reg)
  lvlxl128,   // Load vector left indexed LRU (128-reg)
  lvrxl128,   // Load vector right indexed LRU (128-reg)
  stvlxl128,  // Store vector left indexed LRU (128-reg)
  stvrxl128,  // Store vector right indexed LRU (128-reg)
  lvsl128,    // Load vector for shift left (128-reg)
  lvsr128,    // Load vector for shift right (128-reg)
  lvewx128,   // Load vector element word indexed (128-reg)
  lvxl128,    // Load vector indexed LRU (128-reg)
  stvewx128,  // Store vector element word indexed (128-reg)
  stvxl128,   // Store vector indexed LRU (128-reg)
  vsldoi128,  // Vector shift left double by octet immediate (128-reg)

  // Vector floating-point arithmetic
  vaddfp,     // Vector add floating-point
  vsubfp,     // Vector subtract floating-point
  vmaddfp,    // Vector multiply-add floating-point
  vnmsubfp,   // Vector negative multiply-subtract floating-point
  vmulfp128,  // Vector multiply floating-point (128-reg)
  vrsqrtefp,  // Vector reciprocal square root estimate floating-point
  vrefp,      // Vector reciprocal estimate floating-point
  vlogfp,     // Vector log2 estimate floating-point
  vexptefp,   // Vector 2^x estimate floating-point
  vmaxfp,     // Vector maximum floating-point
  vminfp,     // Vector minimum floating-point

  // VMX128 floating-point arithmetic (Xbox 360 specific)
  vaddfp128,     // Vector add floating-point (128-reg)
  vsubfp128,     // Vector subtract floating-point (128-reg)
  vmaddfp128,    // Vector multiply-add floating-point (128-reg)
  vmaddcfp128,   // Vector multiply-add floating-point (C variant, 128-reg)
  vnmsubfp128,   // Vector negative multiply-subtract floating-point (128-reg)
  vmaxfp128,     // Vector maximum floating-point (128-reg)
  vminfp128,     // Vector minimum floating-point (128-reg)
  vrefp128,      // Vector reciprocal estimate floating-point (128-reg)
  vrsqrtefp128,  // Vector reciprocal square root estimate floating-point (128-reg)
  vexptefp128,   // Vector 2^x estimate floating-point (128-reg)
  vlogefp128,    // Vector log2 estimate floating-point (128-reg)

  // VMX128 dot product instructions (Xbox 360 specific)
  vdot3fp128,   // Vector 3-element dot product (128-reg)
  vdot4fp128,   // Vector 4-element dot product (128-reg)
  vmsum3fp128,  // Vector multiply-sum 3-element (128-reg)
  vmsum4fp128,  // Vector multiply-sum 4-element (128-reg)

  // Vector integer arithmetic
  vaddubm,  // Vector add unsigned byte modulo
  vadduhm,  // Vector add unsigned halfword modulo
  vadduwm,  // Vector add unsigned word modulo
  vsububm,  // Vector subtract unsigned byte modulo
  vsubuhm,  // Vector subtract unsigned halfword modulo
  vsubuwm,  // Vector subtract unsigned word modulo
  vmuloub,  // Vector multiply odd unsigned byte
  vmulouh,  // Vector multiply odd unsigned halfword
  vmulouw,  // Vector multiply odd unsigned word
  vmuleub,  // Vector multiply even unsigned byte
  vmuleuh,  // Vector multiply even unsigned halfword
  vmuleuw,  // Vector multiply even unsigned word
  vavgub,   // Vector average unsigned byte
  vavguh,   // Vector average unsigned halfword
  vavguw,   // Vector average unsigned word

  // Vector logical
  vand,   // Vector AND
  vandc,  // Vector AND with complement
  vor,    // Vector OR
  vorc,   // Vector OR with complement (VMX128)
  vxor,   // Vector XOR
  vnor,   // Vector NOR
  vsel,   // Vector select

  // VMX128 logical (Xbox 360 specific)
  vand128,   // Vector AND (128-reg)
  vandc128,  // Vector AND with complement (128-reg)
  vor128,    // Vector OR (128-reg)
  vxor128,   // Vector XOR (128-reg)
  vnor128,   // Vector NOR (128-reg)
  vsel128,   // Vector select (128-reg)
  vslo128,   // Vector shift left by octet (128-reg)
  vsro128,   // Vector shift right by octet (128-reg)

  // Vector compare floating-point
  vcmpeqfp,   // Vector compare equal-to floating-point
  vcmpgefp,   // Vector compare greater-than-or-equal floating-point
  vcmpgtfp,   // Vector compare greater-than floating-point
  vcmpbfp,    // Vector compare bounds floating-point
  vcmpeqfp_,  // Vector compare equal-to floating-point (Rc=1)
  vcmpgefp_,  // Vector compare greater-than-or-equal floating-point (Rc=1)
  vcmpgtfp_,  // Vector compare greater-than floating-point (Rc=1)

  // Vector compare integer
  vcmpequb,  // Vector compare equal unsigned byte
  vcmpequh,  // Vector compare equal unsigned halfword
  vcmpequw,  // Vector compare equal unsigned word
  vcmpgtub,  // Vector compare greater-than unsigned byte
  vcmpgtuh,  // Vector compare greater-than unsigned halfword
  vcmpgtuw,  // Vector compare greater-than unsigned word
  vcmpgtsb,  // Vector compare greater-than signed byte
  vcmpgtsh,  // Vector compare greater-than signed halfword
  vcmpgtsw,  // Vector compare greater-than signed word

  // VMX128 compare (Xbox 360 specific)
  vcmpeqfp128,  // Vector compare equal-to floating-point (128-reg)
  vcmpgefp128,  // Vector compare greater-than-or-equal floating-point (128-reg)
  vcmpgtfp128,  // Vector compare greater-than floating-point (128-reg)
  vcmpbfp128,   // Vector compare bounds floating-point (128-reg)
  vcmpequw128,  // Vector compare equal unsigned word (128-reg)

  // Vector permute/merge
  vperm,     // Vector permute
  vperm128,  // Vector permute (128-reg)
  vmrghb,    // Vector merge high byte
  vmrghh,    // Vector merge high halfword
  vmrghw,    // Vector merge high word
  vmrglb,    // Vector merge low byte
  vmrglh,    // Vector merge low halfword
  vmrglw,    // Vector merge low word

  // VMX128 merge (Xbox 360 specific)
  vmrghw128,   // Vector merge high word (128-reg)
  vmrglw128,   // Vector merge low word (128-reg)
  vpermwi128,  // Vector permute word immediate (128-reg)

  // Vector pack/unpack
  vpkuhum,  // Vector pack unsigned halfword unsigned modulo
  vpkuwum,  // Vector pack unsigned word unsigned modulo
  vpkuhus,  // Vector pack unsigned halfword unsigned saturate
  vpkuwus,  // Vector pack unsigned word unsigned saturate
  vpkshus,  // Vector pack signed halfword unsigned saturate
  vpkswus,  // Vector pack signed word unsigned saturate
  vpkshss,  // Vector pack signed halfword signed saturate
  vpkswss,  // Vector pack signed word signed saturate
  vupkhsb,  // Vector unpack high signed byte
  vupkhsh,  // Vector unpack high signed halfword
  vupklsb,  // Vector unpack low signed byte
  vupklsh,  // Vector unpack low signed halfword

  // VMX128 pack (Xbox 360 specific)
  vpkshss128,  // Vector pack signed halfword signed saturate (128-reg)
  vpkshus128,  // Vector pack signed halfword unsigned saturate (128-reg)
  vpkswss128,  // Vector pack signed word signed saturate (128-reg)
  vpkswus128,  // Vector pack signed word unsigned saturate (128-reg)
  vpkuhum128,  // Vector pack unsigned halfword unsigned modulo (128-reg)
  vpkuhus128,  // Vector pack unsigned halfword unsigned saturate (128-reg)
  vpkuwum128,  // Vector pack unsigned word unsigned modulo (128-reg)
  vpkuwus128,  // Vector pack unsigned word unsigned saturate (128-reg)
  vupkhsb128,  // Vector unpack high signed byte (128-reg)
  vupklsb128,  // Vector unpack low signed byte (128-reg)

  // Vector splat
  vspltb,    // Vector splat byte
  vsplth,    // Vector splat halfword
  vspltw,    // Vector splat word
  vspltisb,  // Vector splat immediate signed byte
  vspltish,  // Vector splat immediate signed halfword
  vspltisw,  // Vector splat immediate signed word

  // VMX128 splat (Xbox 360 specific)
  vspltw128,    // Vector splat word (128-reg)
  vspltisw128,  // Vector splat immediate signed word (128-reg)

  // Vector shift/rotate
  vslb,   // Vector shift left byte
  vslh,   // Vector shift left halfword
  vslw,   // Vector shift left word
  vsrb,   // Vector shift right byte
  vsrh,   // Vector shift right halfword
  vsrw,   // Vector shift right word
  vsrab,  // Vector shift right algebraic byte
  vsrah,  // Vector shift right algebraic halfword
  vsraw,  // Vector shift right algebraic word
  vrlb,   // Vector rotate left byte
  vrlh,   // Vector rotate left halfword
  vrlw,   // Vector rotate left word
  vsl,    // Vector shift left (128-bit)
  vsr,    // Vector shift right (128-bit)
  vslo,   // Vector shift left by octet
  vsro,   // Vector shift right by octet

  // Vector conversion
  vcfux,   // Vector convert from unsigned fixed-point word
  vcfsx,   // Vector convert from signed fixed-point word
  vctuxs,  // Vector convert to unsigned fixed-point word saturate
  vctsxs,  // Vector convert to signed fixed-point word saturate
  vrfin,   // Vector round to floating-point integer nearest
  vrfiz,   // Vector round to floating-point integer toward zero
  vrfip,   // Vector round to floating-point integer toward +infinity
  vrfim,   // Vector round to floating-point integer toward -infinity

  // VMX128 conversion (Xbox 360 specific)
  vcfpsxws128,  // Vector convert from FP to signed fixed-point word saturate (128-reg)
  vcfpuxws128,  // Vector convert from FP to unsigned fixed-point word saturate (128-reg)
  vcsxwfp128,   // Vector convert from signed fixed-point word to FP (128-reg)
  vcuxwfp128,   // Vector convert from unsigned fixed-point word to FP (128-reg)
  vrfim128,     // Vector round to FP integer toward -infinity (128-reg)
  vrfin128,     // Vector round to FP integer nearest (128-reg)
  vrfip128,     // Vector round to FP integer toward +infinity (128-reg)
  vrfiz128,     // Vector round to FP integer toward zero (128-reg)

  // VMX128 move/misc
  vmrgow128,   // Vector merge odd word (128-reg)
  vmrgew128,   // Vector merge even word (128-reg)
  vrlw128,     // Vector rotate left word (128-reg)
  vslw128,     // Vector shift left word (128-reg)
  vsrw128,     // Vector shift right word (128-reg)
  vsraw128,    // Vector shift right algebraic word (128-reg)
  vupkd3d128,  // Vector unpack D3D format (128-reg)
  vpkd3d128,   // Vector pack D3D format (128-reg)
  vrlimi128,   // Vector rotate left immediate and mask insert (128-reg)

  // Vector status/control
  mfvscr,  // Move from vector status and control register
  mtvscr,  // Move to vector status and control register
};

/**
 * Opcode groups for classification
 */
enum class OpcodeGroup : uint8_t {
  kGeneral,  // General ALU/logical
  kBranch,   // Branch/control flow
  kMemory,   // Load/store
  kSpecial,  // SPR access
  kSync,     // Synchronization
  kSystem,   // System call/trap
  kFloat,    // Floating-point
  kVector,   // VMX/VMX128 vector
};

/**
 * Check if opcode is a branch instruction
 * Used for basic block boundary detection
 */
inline bool is_branch_instruction(Opcode op) {
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
      return true;
    default:
      return false;
  }
}

/**
 * Check if opcode is an unconditional branch (always taken)
 * Used for control flow analysis
 */
inline bool is_unconditional_branch(Opcode op) {
  switch (op) {
    case Opcode::b:
    case Opcode::ba:
    case Opcode::bl:
    case Opcode::bla:
      return true;
    default:
      return false;
  }
}

/**
 * Check if opcode terminates a basic block (branch or return)
 * Used for control flow analysis
 */
inline bool is_terminator_instruction(Opcode op) {
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
    case Opcode::sc:   // System call
    case Opcode::tw:   // Trap word
    case Opcode::twi:  // Trap word immediate
      return true;
    default:
      return false;
  }
}

/**
 * Opcode information structure
 */
struct OpcodeInfo {
  Opcode opcode;
  InstrFormat format;
  OpcodeGroup group;
  const char* name;
  uint32_t primary_opcode;   // Bits 0-5
  uint32_t extended_opcode;  // Bits 21-30 (or other extended field)
  bool has_extended;         // True if uses extended opcode
};

/**
 * Lookup opcode from instruction code
 * @param code Raw 32-bit instruction (big-endian)
 * @return Opcode enum value
 */
Opcode lookup_opcode(uint32_t code);

/**
 * Get opcode information
 * @param opcode Opcode enum value
 * @return Opcode information structure
 */
const OpcodeInfo& get_opcode_info(Opcode opcode);

}  // namespace rex::codegen::ppc
