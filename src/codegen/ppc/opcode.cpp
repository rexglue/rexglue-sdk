/**
 * @file        rexcodegen/internal/ppc/opcodes.cpp
 * @brief       PPC opcode table implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "opcode.h"

#include <array>
#include <mutex>
#include <unordered_map>

#include <rex/types.h>

using namespace rex;

namespace rex::codegen::ppc {

// Helper to extract bit fields from instruction
constexpr u32 extract_bits(u32 value, u32 start, u32 count) {
  return (value >> (32 - start - count)) & ((1u << count) - 1);
}

//=============================================================================
// Opcode information table
//=============================================================================

static const std::array<OpcodeInfo, 320> g_opcode_table = {{
    // Primary opcode 16: bcx (conditional branch) - all variants
    {Opcode::bc, InstrFormat::kB, OpcodeGroup::kBranch, "bc", 16, 0, false},
    {Opcode::bca, InstrFormat::kB, OpcodeGroup::kBranch, "bca", 16, 0, false},
    {Opcode::bcl, InstrFormat::kB, OpcodeGroup::kBranch, "bcl", 16, 0, false},
    {Opcode::bcla, InstrFormat::kB, OpcodeGroup::kBranch, "bcla", 16, 0, false},

    // Primary opcode 18: bx (unconditional branch) - all variants
    {Opcode::b, InstrFormat::kI, OpcodeGroup::kBranch, "b", 18, 0, false},
    {Opcode::ba, InstrFormat::kI, OpcodeGroup::kBranch, "ba", 18, 0, false},
    {Opcode::bl, InstrFormat::kI, OpcodeGroup::kBranch, "bl", 18, 0, false},
    {Opcode::bla, InstrFormat::kI, OpcodeGroup::kBranch, "bla", 18, 0, false},

    // Primary opcode 19: Extended branch instructions - all variants
    {Opcode::bclr, InstrFormat::kXL, OpcodeGroup::kBranch, "bclr", 19, 16, true},
    {Opcode::bclrl, InstrFormat::kXL, OpcodeGroup::kBranch, "bclrl", 19, 16, true},
    {Opcode::bcctr, InstrFormat::kXL, OpcodeGroup::kBranch, "bcctr", 19, 528, true},
    {Opcode::bcctrl, InstrFormat::kXL, OpcodeGroup::kBranch, "bcctrl", 19, 528, true},

    // Primary opcode 14: addi
    {Opcode::addi, InstrFormat::kD, OpcodeGroup::kGeneral, "addi", 14, 0, false},

    // Primary opcode 15: addis
    {Opcode::addis, InstrFormat::kD, OpcodeGroup::kGeneral, "addis", 15, 0, false},

    // Primary opcode 24: ori
    {Opcode::ori, InstrFormat::kD, OpcodeGroup::kGeneral, "ori", 24, 0, false},

    // Primary opcode 25: oris
    {Opcode::oris, InstrFormat::kD, OpcodeGroup::kGeneral, "oris", 25, 0, false},

    // Primary opcode 26: xori
    {Opcode::xori, InstrFormat::kD, OpcodeGroup::kGeneral, "xori", 26, 0, false},

    // Primary opcode 27: xoris
    {Opcode::xoris, InstrFormat::kD, OpcodeGroup::kGeneral, "xoris", 27, 0, false},

    // Primary opcode 28: andi.
    {Opcode::andi_, InstrFormat::kD, OpcodeGroup::kGeneral, "andi.", 28, 0, false},

    // Primary opcode 29: andis.
    {Opcode::andis_, InstrFormat::kD, OpcodeGroup::kGeneral, "andis.", 29, 0, false},

    // Primary opcode 32: lwz
    {Opcode::lwz, InstrFormat::kD, OpcodeGroup::kMemory, "lwz", 32, 0, false},

    // Primary opcode 33: lwzu
    {Opcode::lwzu, InstrFormat::kD, OpcodeGroup::kMemory, "lwzu", 33, 0, false},

    // Primary opcode 34: lbz
    {Opcode::lbz, InstrFormat::kD, OpcodeGroup::kMemory, "lbz", 34, 0, false},

    // Primary opcode 35: lbzu
    {Opcode::lbzu, InstrFormat::kD, OpcodeGroup::kMemory, "lbzu", 35, 0, false},

    // Primary opcode 36: stw
    {Opcode::stw, InstrFormat::kD, OpcodeGroup::kMemory, "stw", 36, 0, false},

    // Primary opcode 37: stwu
    {Opcode::stwu, InstrFormat::kD, OpcodeGroup::kMemory, "stwu", 37, 0, false},

    // Primary opcode 38: stb
    {Opcode::stb, InstrFormat::kD, OpcodeGroup::kMemory, "stb", 38, 0, false},

    // Primary opcode 39: stbu
    {Opcode::stbu, InstrFormat::kD, OpcodeGroup::kMemory, "stbu", 39, 0, false},

    // Primary opcode 40: lhz
    {Opcode::lhz, InstrFormat::kD, OpcodeGroup::kMemory, "lhz", 40, 0, false},

    // Primary opcode 41: lhzu
    {Opcode::lhzu, InstrFormat::kD, OpcodeGroup::kMemory, "lhzu", 41, 0, false},

    // Primary opcode 44: sth
    {Opcode::sth, InstrFormat::kD, OpcodeGroup::kMemory, "sth", 44, 0, false},

    // Primary opcode 45: sthu
    {Opcode::sthu, InstrFormat::kD, OpcodeGroup::kMemory, "sthu", 45, 0, false},

    // Primary opcode 58: ld, ldu (DS format with XO)
    {Opcode::ld, InstrFormat::kDS, OpcodeGroup::kMemory, "ld", 58, 0, true},
    {Opcode::ldu, InstrFormat::kDS, OpcodeGroup::kMemory, "ldu", 58, 1, true},

    // Primary opcode 62: std, stdu (DS format with XO)
    {Opcode::std, InstrFormat::kDS, OpcodeGroup::kMemory, "std", 62, 0, true},
    {Opcode::stdu, InstrFormat::kDS, OpcodeGroup::kMemory, "stdu", 62, 1, true},

    // Primary opcode 31: Extended instructions (many ALU/logical operations)
    {Opcode::cmp, InstrFormat::kX, OpcodeGroup::kGeneral, "cmp", 31, 0, true},
    {Opcode::cmpl, InstrFormat::kX, OpcodeGroup::kGeneral, "cmpl", 31, 32, true},
    {Opcode::tw, InstrFormat::kX, OpcodeGroup::kSystem, "tw", 31, 4, true},
    {Opcode::subf, InstrFormat::kXO, OpcodeGroup::kGeneral, "subf", 31, 40, true},
    {Opcode::neg, InstrFormat::kXO, OpcodeGroup::kGeneral, "neg", 31, 104, true},
    {Opcode::and_, InstrFormat::kX, OpcodeGroup::kGeneral, "and", 31, 28, true},
    {Opcode::or_, InstrFormat::kX, OpcodeGroup::kGeneral, "or", 31, 444, true},
    {Opcode::xor_, InstrFormat::kX, OpcodeGroup::kGeneral, "xor", 31, 316, true},
    {Opcode::nand, InstrFormat::kX, OpcodeGroup::kGeneral, "nand", 31, 476, true},
    {Opcode::nor, InstrFormat::kX, OpcodeGroup::kGeneral, "nor", 31, 124, true},
    {Opcode::add, InstrFormat::kXO, OpcodeGroup::kGeneral, "add", 31, 266, true},
    {Opcode::slw, InstrFormat::kX, OpcodeGroup::kGeneral, "slw", 31, 24, true},
    {Opcode::srw, InstrFormat::kX, OpcodeGroup::kGeneral, "srw", 31, 536, true},
    {Opcode::sraw, InstrFormat::kX, OpcodeGroup::kGeneral, "sraw", 31, 792, true},
    {Opcode::mfspr, InstrFormat::kXFX, OpcodeGroup::kSpecial, "mfspr", 31, 339, true},
    {Opcode::mtspr, InstrFormat::kXFX, OpcodeGroup::kSpecial, "mtspr", 31, 467, true},
    // Simplified mnemonics for SPR access (synthetic opcodes from post-decode)
    {Opcode::mflr, InstrFormat::kXFX, OpcodeGroup::kSpecial, "mflr", 0, 0, false},
    {Opcode::mtlr, InstrFormat::kXFX, OpcodeGroup::kSpecial, "mtlr", 0, 0, false},
    {Opcode::mfctr, InstrFormat::kXFX, OpcodeGroup::kSpecial, "mfctr", 0, 0, false},
    {Opcode::mtctr, InstrFormat::kXFX, OpcodeGroup::kSpecial, "mtctr", 0, 0, false},
    {Opcode::mfcr, InstrFormat::kX, OpcodeGroup::kSpecial, "mfcr", 31, 19, true},
    {Opcode::mtcr, InstrFormat::kXFX, OpcodeGroup::kSpecial, "mtcrf", 31, 144, true},
    {Opcode::sync, InstrFormat::kX, OpcodeGroup::kSync, "sync", 31, 598, true},
    {Opcode::isync, InstrFormat::kXL, OpcodeGroup::kSync, "isync", 19, 150, true},

    // Primary opcode 11: cmpi
    {Opcode::cmpi, InstrFormat::kD, OpcodeGroup::kGeneral, "cmpi", 11, 0, false},

    // Primary opcode 10: cmpli
    {Opcode::cmpli, InstrFormat::kD, OpcodeGroup::kGeneral, "cmpli", 10, 0, false},

    // Primary opcode 21: rlwinm
    {Opcode::rlwinm, InstrFormat::kM, OpcodeGroup::kGeneral, "rlwinm", 21, 0, false},

    // Primary opcode 23: rlwnm
    {Opcode::rlwnm, InstrFormat::kM, OpcodeGroup::kGeneral, "rlwnm", 23, 0, false},

    // Primary opcode 17: sc
    {Opcode::sc, InstrFormat::kX, OpcodeGroup::kSystem, "sc", 17, 0, false},

    // Primary opcode 3: twi
    {Opcode::twi, InstrFormat::kD, OpcodeGroup::kSystem, "twi", 3, 0, false},

    //=========================================================================
    // Floating-Point Load/Store
    //=========================================================================
    {Opcode::lfs, InstrFormat::kD, OpcodeGroup::kFloat, "lfs", 48, 0, false},
    {Opcode::lfsu, InstrFormat::kD, OpcodeGroup::kFloat, "lfsu", 49, 0, false},
    {Opcode::lfd, InstrFormat::kD, OpcodeGroup::kFloat, "lfd", 50, 0, false},
    {Opcode::lfdu, InstrFormat::kD, OpcodeGroup::kFloat, "lfdu", 51, 0, false},
    {Opcode::stfs, InstrFormat::kD, OpcodeGroup::kFloat, "stfs", 52, 0, false},
    {Opcode::stfsu, InstrFormat::kD, OpcodeGroup::kFloat, "stfsu", 53, 0, false},
    {Opcode::stfd, InstrFormat::kD, OpcodeGroup::kFloat, "stfd", 54, 0, false},
    {Opcode::stfdu, InstrFormat::kD, OpcodeGroup::kFloat, "stfdu", 55, 0, false},
    {Opcode::lfsx, InstrFormat::kX, OpcodeGroup::kFloat, "lfsx", 31, 535, true},
    {Opcode::lfdx, InstrFormat::kX, OpcodeGroup::kFloat, "lfdx", 31, 599, true},
    {Opcode::stfsx, InstrFormat::kX, OpcodeGroup::kFloat, "stfsx", 31, 663, true},
    {Opcode::stfdx, InstrFormat::kX, OpcodeGroup::kFloat, "stfdx", 31, 727, true},

    //=========================================================================
    // Floating-Point Arithmetic (Primary 59 - Single Precision)
    //=========================================================================
    {Opcode::fadds, InstrFormat::kX, OpcodeGroup::kFloat, "fadds", 59, 21, true},
    {Opcode::fsubs, InstrFormat::kX, OpcodeGroup::kFloat, "fsubs", 59, 20, true},
    {Opcode::fmuls, InstrFormat::kX, OpcodeGroup::kFloat, "fmuls", 59, 25, true},
    {Opcode::fdivs, InstrFormat::kX, OpcodeGroup::kFloat, "fdivs", 59, 18, true},
    {Opcode::fsqrts, InstrFormat::kX, OpcodeGroup::kFloat, "fsqrts", 59, 22, true},
    {Opcode::fres, InstrFormat::kX, OpcodeGroup::kFloat, "fres", 59, 24, true},
    {Opcode::frsqrtes, InstrFormat::kX, OpcodeGroup::kFloat, "frsqrtes", 59, 26, true},
    {Opcode::fmadds, InstrFormat::kX, OpcodeGroup::kFloat, "fmadds", 59, 29, true},
    {Opcode::fmsubs, InstrFormat::kX, OpcodeGroup::kFloat, "fmsubs", 59, 28, true},
    {Opcode::fnmadds, InstrFormat::kX, OpcodeGroup::kFloat, "fnmadds", 59, 31, true},
    {Opcode::fnmsubs, InstrFormat::kX, OpcodeGroup::kFloat, "fnmsubs", 59, 30, true},

    //=========================================================================
    // Floating-Point Arithmetic (Primary 63 - Double Precision)
    //=========================================================================
    {Opcode::fadd, InstrFormat::kX, OpcodeGroup::kFloat, "fadd", 63, 21, true},
    {Opcode::fsub, InstrFormat::kX, OpcodeGroup::kFloat, "fsub", 63, 20, true},
    {Opcode::fmul, InstrFormat::kX, OpcodeGroup::kFloat, "fmul", 63, 25, true},
    {Opcode::fdiv, InstrFormat::kX, OpcodeGroup::kFloat, "fdiv", 63, 18, true},
    {Opcode::fsqrt, InstrFormat::kX, OpcodeGroup::kFloat, "fsqrt", 63, 22, true},
    {Opcode::fre, InstrFormat::kX, OpcodeGroup::kFloat, "fre", 63, 24, true},
    {Opcode::frsqrte, InstrFormat::kX, OpcodeGroup::kFloat, "frsqrte", 63, 26, true},
    {Opcode::fmadd, InstrFormat::kX, OpcodeGroup::kFloat, "fmadd", 63, 29, true},
    {Opcode::fmsub, InstrFormat::kX, OpcodeGroup::kFloat, "fmsub", 63, 28, true},
    {Opcode::fnmadd, InstrFormat::kX, OpcodeGroup::kFloat, "fnmadd", 63, 31, true},
    {Opcode::fnmsub, InstrFormat::kX, OpcodeGroup::kFloat, "fnmsub", 63, 30, true},
    {Opcode::fsel, InstrFormat::kX, OpcodeGroup::kFloat, "fsel", 63, 23, true},

    //=========================================================================
    // Floating-Point Move/Misc
    //=========================================================================
    {Opcode::fmr, InstrFormat::kX, OpcodeGroup::kFloat, "fmr", 63, 72, true},
    {Opcode::fneg, InstrFormat::kX, OpcodeGroup::kFloat, "fneg", 63, 40, true},
    {Opcode::fabs, InstrFormat::kX, OpcodeGroup::kFloat, "fabs", 63, 264, true},
    {Opcode::fnabs, InstrFormat::kX, OpcodeGroup::kFloat, "fnabs", 63, 136, true},

    //=========================================================================
    // Floating-Point Conversion
    //=========================================================================
    {Opcode::frsp, InstrFormat::kX, OpcodeGroup::kFloat, "frsp", 63, 12, true},
    {Opcode::fctiw, InstrFormat::kX, OpcodeGroup::kFloat, "fctiw", 63, 14, true},
    {Opcode::fctiwz, InstrFormat::kX, OpcodeGroup::kFloat, "fctiwz", 63, 15, true},
    {Opcode::fctid, InstrFormat::kX, OpcodeGroup::kFloat, "fctid", 63, 814, true},
    {Opcode::fctidz, InstrFormat::kX, OpcodeGroup::kFloat, "fctidz", 63, 815, true},
    {Opcode::fcfid, InstrFormat::kX, OpcodeGroup::kFloat, "fcfid", 63, 846, true},

    //=========================================================================
    // Floating-Point Compare
    //=========================================================================
    {Opcode::fcmpu, InstrFormat::kX, OpcodeGroup::kFloat, "fcmpu", 63, 0, true},
    {Opcode::fcmpo, InstrFormat::kX, OpcodeGroup::kFloat, "fcmpo", 63, 32, true},

    //=========================================================================
    // Floating-Point Status/Control
    //=========================================================================
    {Opcode::mffs, InstrFormat::kX, OpcodeGroup::kFloat, "mffs", 63, 583, true},
    {Opcode::mtfsf, InstrFormat::kX, OpcodeGroup::kFloat, "mtfsf", 63, 711, true},
    {Opcode::mtfsfi, InstrFormat::kX, OpcodeGroup::kFloat, "mtfsfi", 63, 134, true},
    {Opcode::mtfsb0, InstrFormat::kX, OpcodeGroup::kFloat, "mtfsb0", 63, 70, true},
    {Opcode::mtfsb1, InstrFormat::kX, OpcodeGroup::kFloat, "mtfsb1", 63, 38, true},

    //=========================================================================
    // VMX Load/Store
    //=========================================================================
    {Opcode::lvx, InstrFormat::kX, OpcodeGroup::kVector, "lvx", 4, 103, true},
    {Opcode::lvxl, InstrFormat::kX, OpcodeGroup::kVector, "lvxl", 4, 359, true},
    {Opcode::stvx, InstrFormat::kX, OpcodeGroup::kVector, "stvx", 4, 231, true},
    {Opcode::stvxl, InstrFormat::kX, OpcodeGroup::kVector, "stvxl", 4, 487, true},
    {Opcode::lvlx, InstrFormat::kX, OpcodeGroup::kVector, "lvlx", 4, 39, true},
    {Opcode::lvrx, InstrFormat::kX, OpcodeGroup::kVector, "lvrx", 4, 71, true},
    {Opcode::stvlx, InstrFormat::kX, OpcodeGroup::kVector, "stvlx", 4, 167, true},
    {Opcode::stvrx, InstrFormat::kX, OpcodeGroup::kVector, "stvrx", 4, 199, true},
    {Opcode::lvsl, InstrFormat::kX, OpcodeGroup::kVector, "lvsl", 4, 6, true},
    {Opcode::lvsr, InstrFormat::kX, OpcodeGroup::kVector, "lvsr", 4, 38, true},

    //=========================================================================
    // VMX Floating-Point Arithmetic
    //=========================================================================
    {Opcode::vaddfp, InstrFormat::kX, OpcodeGroup::kVector, "vaddfp", 4, 10, true},
    {Opcode::vsubfp, InstrFormat::kX, OpcodeGroup::kVector, "vsubfp", 4, 74, true},
    {Opcode::vmaddfp, InstrFormat::kX, OpcodeGroup::kVector, "vmaddfp", 4, 32, true},
    {Opcode::vnmsubfp, InstrFormat::kX, OpcodeGroup::kVector, "vnmsubfp", 4, 33, true},
    {Opcode::vmaxfp, InstrFormat::kX, OpcodeGroup::kVector, "vmaxfp", 4, 1034, true},
    {Opcode::vminfp, InstrFormat::kX, OpcodeGroup::kVector, "vminfp", 4, 1098, true},
    {Opcode::vrsqrtefp, InstrFormat::kX, OpcodeGroup::kVector, "vrsqrtefp", 4, 330, true},
    {Opcode::vrefp, InstrFormat::kX, OpcodeGroup::kVector, "vrefp", 4, 266, true},
    {Opcode::vlogfp, InstrFormat::kX, OpcodeGroup::kVector, "vlogfp", 4, 458, true},
    {Opcode::vexptefp, InstrFormat::kX, OpcodeGroup::kVector, "vexptefp", 4, 394, true},

    //=========================================================================
    // VMX Integer Arithmetic
    //=========================================================================
    {Opcode::vaddubm, InstrFormat::kX, OpcodeGroup::kVector, "vaddubm", 4, 0, true},
    {Opcode::vadduhm, InstrFormat::kX, OpcodeGroup::kVector, "vadduhm", 4, 64, true},
    {Opcode::vadduwm, InstrFormat::kX, OpcodeGroup::kVector, "vadduwm", 4, 128, true},
    {Opcode::vsububm, InstrFormat::kX, OpcodeGroup::kVector, "vsububm", 4, 1024, true},
    {Opcode::vsubuhm, InstrFormat::kX, OpcodeGroup::kVector, "vsubuhm", 4, 1088, true},
    {Opcode::vsubuwm, InstrFormat::kX, OpcodeGroup::kVector, "vsubuwm", 4, 1152, true},
    {Opcode::vmuloub, InstrFormat::kX, OpcodeGroup::kVector, "vmuloub", 4, 8, true},
    {Opcode::vmulouh, InstrFormat::kX, OpcodeGroup::kVector, "vmulouh", 4, 72, true},
    {Opcode::vmuleub, InstrFormat::kX, OpcodeGroup::kVector, "vmuleub", 4, 264, true},
    {Opcode::vmuleuh, InstrFormat::kX, OpcodeGroup::kVector, "vmuleuh", 4, 328, true},
    {Opcode::vavgub, InstrFormat::kX, OpcodeGroup::kVector, "vavgub", 4, 1026, true},
    {Opcode::vavguh, InstrFormat::kX, OpcodeGroup::kVector, "vavguh", 4, 1090, true},
    {Opcode::vavguw, InstrFormat::kX, OpcodeGroup::kVector, "vavguw", 4, 1154, true},

    //=========================================================================
    // VMX Logical
    //=========================================================================
    {Opcode::vand, InstrFormat::kX, OpcodeGroup::kVector, "vand", 4, 1028, true},
    {Opcode::vandc, InstrFormat::kX, OpcodeGroup::kVector, "vandc", 4, 1092, true},
    {Opcode::vor, InstrFormat::kX, OpcodeGroup::kVector, "vor", 4, 1156, true},
    {Opcode::vxor, InstrFormat::kX, OpcodeGroup::kVector, "vxor", 4, 1220, true},
    {Opcode::vnor, InstrFormat::kX, OpcodeGroup::kVector, "vnor", 4, 1284, true},
    {Opcode::vsel, InstrFormat::kX, OpcodeGroup::kVector, "vsel", 4, 42, true},

    //=========================================================================
    // VMX Compare (Floating-Point)
    //=========================================================================
    {Opcode::vcmpeqfp, InstrFormat::kX, OpcodeGroup::kVector, "vcmpeqfp", 4, 198, true},
    {Opcode::vcmpgefp, InstrFormat::kX, OpcodeGroup::kVector, "vcmpgefp", 4, 454, true},
    {Opcode::vcmpgtfp, InstrFormat::kX, OpcodeGroup::kVector, "vcmpgtfp", 4, 710, true},
    {Opcode::vcmpbfp, InstrFormat::kX, OpcodeGroup::kVector, "vcmpbfp", 4, 966, true},
    {Opcode::vcmpeqfp_, InstrFormat::kX, OpcodeGroup::kVector, "vcmpeqfp.", 4, 198, true},
    {Opcode::vcmpgefp_, InstrFormat::kX, OpcodeGroup::kVector, "vcmpgefp.", 4, 454, true},
    {Opcode::vcmpgtfp_, InstrFormat::kX, OpcodeGroup::kVector, "vcmpgtfp.", 4, 710, true},

    //=========================================================================
    // VMX Compare (Integer)
    //=========================================================================
    {Opcode::vcmpequb, InstrFormat::kX, OpcodeGroup::kVector, "vcmpequb", 4, 6, true},
    {Opcode::vcmpequh, InstrFormat::kX, OpcodeGroup::kVector, "vcmpequh", 4, 70, true},
    {Opcode::vcmpequw, InstrFormat::kX, OpcodeGroup::kVector, "vcmpequw", 4, 134, true},
    {Opcode::vcmpgtub, InstrFormat::kX, OpcodeGroup::kVector, "vcmpgtub", 4, 518, true},
    {Opcode::vcmpgtuh, InstrFormat::kX, OpcodeGroup::kVector, "vcmpgtuh", 4, 582, true},
    {Opcode::vcmpgtuw, InstrFormat::kX, OpcodeGroup::kVector, "vcmpgtuw", 4, 646, true},
    {Opcode::vcmpgtsb, InstrFormat::kX, OpcodeGroup::kVector, "vcmpgtsb", 4, 774, true},
    {Opcode::vcmpgtsh, InstrFormat::kX, OpcodeGroup::kVector, "vcmpgtsh", 4, 838, true},
    {Opcode::vcmpgtsw, InstrFormat::kX, OpcodeGroup::kVector, "vcmpgtsw", 4, 902, true},

    //=========================================================================
    // VMX Permute/Merge
    //=========================================================================
    {Opcode::vperm, InstrFormat::kX, OpcodeGroup::kVector, "vperm", 4, 43, true},
    {Opcode::vmrghb, InstrFormat::kX, OpcodeGroup::kVector, "vmrghb", 4, 12, true},
    {Opcode::vmrghh, InstrFormat::kX, OpcodeGroup::kVector, "vmrghh", 4, 76, true},
    {Opcode::vmrghw, InstrFormat::kX, OpcodeGroup::kVector, "vmrghw", 4, 140, true},
    {Opcode::vmrglb, InstrFormat::kX, OpcodeGroup::kVector, "vmrglb", 4, 268, true},
    {Opcode::vmrglh, InstrFormat::kX, OpcodeGroup::kVector, "vmrglh", 4, 332, true},
    {Opcode::vmrglw, InstrFormat::kX, OpcodeGroup::kVector, "vmrglw", 4, 396, true},

    //=========================================================================
    // VMX Pack/Unpack
    //=========================================================================
    {Opcode::vpkuhum, InstrFormat::kX, OpcodeGroup::kVector, "vpkuhum", 4, 14, true},
    {Opcode::vpkuwum, InstrFormat::kX, OpcodeGroup::kVector, "vpkuwum", 4, 78, true},
    {Opcode::vpkuhus, InstrFormat::kX, OpcodeGroup::kVector, "vpkuhus", 4, 142, true},
    {Opcode::vpkuwus, InstrFormat::kX, OpcodeGroup::kVector, "vpkuwus", 4, 206, true},
    {Opcode::vpkshus, InstrFormat::kX, OpcodeGroup::kVector, "vpkshus", 4, 270, true},
    {Opcode::vpkswus, InstrFormat::kX, OpcodeGroup::kVector, "vpkswus", 4, 334, true},
    {Opcode::vpkshss, InstrFormat::kX, OpcodeGroup::kVector, "vpkshss", 4, 398, true},
    {Opcode::vpkswss, InstrFormat::kX, OpcodeGroup::kVector, "vpkswss", 4, 462, true},
    {Opcode::vupkhsb, InstrFormat::kX, OpcodeGroup::kVector, "vupkhsb", 4, 526, true},
    {Opcode::vupkhsh, InstrFormat::kX, OpcodeGroup::kVector, "vupkhsh", 4, 590, true},
    {Opcode::vupklsb, InstrFormat::kX, OpcodeGroup::kVector, "vupklsb", 4, 654, true},
    {Opcode::vupklsh, InstrFormat::kX, OpcodeGroup::kVector, "vupklsh", 4, 718, true},

    //=========================================================================
    // VMX Splat
    //=========================================================================
    {Opcode::vspltb, InstrFormat::kX, OpcodeGroup::kVector, "vspltb", 4, 524, true},
    {Opcode::vsplth, InstrFormat::kX, OpcodeGroup::kVector, "vsplth", 4, 588, true},
    {Opcode::vspltw, InstrFormat::kX, OpcodeGroup::kVector, "vspltw", 4, 652, true},
    {Opcode::vspltisb, InstrFormat::kX, OpcodeGroup::kVector, "vspltisb", 4, 780, true},
    {Opcode::vspltish, InstrFormat::kX, OpcodeGroup::kVector, "vspltish", 4, 844, true},
    {Opcode::vspltisw, InstrFormat::kX, OpcodeGroup::kVector, "vspltisw", 4, 908, true},

    //=========================================================================
    // VMX Shift/Rotate
    //=========================================================================
    {Opcode::vslb, InstrFormat::kX, OpcodeGroup::kVector, "vslb", 4, 260, true},
    {Opcode::vslh, InstrFormat::kX, OpcodeGroup::kVector, "vslh", 4, 324, true},
    {Opcode::vslw, InstrFormat::kX, OpcodeGroup::kVector, "vslw", 4, 388, true},
    {Opcode::vsrb, InstrFormat::kX, OpcodeGroup::kVector, "vsrb", 4, 516, true},
    {Opcode::vsrh, InstrFormat::kX, OpcodeGroup::kVector, "vsrh", 4, 580, true},
    {Opcode::vsrw, InstrFormat::kX, OpcodeGroup::kVector, "vsrw", 4, 644, true},
    {Opcode::vsrab, InstrFormat::kX, OpcodeGroup::kVector, "vsrab", 4, 772, true},
    {Opcode::vsrah, InstrFormat::kX, OpcodeGroup::kVector, "vsrah", 4, 836, true},
    {Opcode::vsraw, InstrFormat::kX, OpcodeGroup::kVector, "vsraw", 4, 900, true},
    {Opcode::vrlb, InstrFormat::kX, OpcodeGroup::kVector, "vrlb", 4, 4, true},
    {Opcode::vrlh, InstrFormat::kX, OpcodeGroup::kVector, "vrlh", 4, 68, true},
    {Opcode::vrlw, InstrFormat::kX, OpcodeGroup::kVector, "vrlw", 4, 132, true},
    {Opcode::vsl, InstrFormat::kX, OpcodeGroup::kVector, "vsl", 4, 452, true},
    {Opcode::vsr, InstrFormat::kX, OpcodeGroup::kVector, "vsr", 4, 708, true},
    {Opcode::vslo, InstrFormat::kX, OpcodeGroup::kVector, "vslo", 4, 1036, true},
    {Opcode::vsro, InstrFormat::kX, OpcodeGroup::kVector, "vsro", 4, 1100, true},

    //=========================================================================
    // VMX Conversion
    //=========================================================================
    {Opcode::vcfux, InstrFormat::kX, OpcodeGroup::kVector, "vcfux", 4, 778, true},
    {Opcode::vcfsx, InstrFormat::kX, OpcodeGroup::kVector, "vcfsx", 4, 842, true},
    {Opcode::vctuxs, InstrFormat::kX, OpcodeGroup::kVector, "vctuxs", 4, 906, true},
    {Opcode::vctsxs, InstrFormat::kX, OpcodeGroup::kVector, "vctsxs", 4, 970, true},
    {Opcode::vrfin, InstrFormat::kX, OpcodeGroup::kVector, "vrfin", 4, 522, true},
    {Opcode::vrfiz, InstrFormat::kX, OpcodeGroup::kVector, "vrfiz", 4, 586, true},
    {Opcode::vrfip, InstrFormat::kX, OpcodeGroup::kVector, "vrfip", 4, 650, true},
    {Opcode::vrfim, InstrFormat::kX, OpcodeGroup::kVector, "vrfim", 4, 714, true},

    //=========================================================================
    // VMX Status/Control
    //=========================================================================
    {Opcode::mfvscr, InstrFormat::kX, OpcodeGroup::kVector, "mfvscr", 4, 1540, true},
    {Opcode::mtvscr, InstrFormat::kX, OpcodeGroup::kVector, "mtvscr", 4, 1604, true},

    //=========================================================================
    // VMX128 (Xbox 360 Extensions)
    //=========================================================================
    {Opcode::lvx128, InstrFormat::kX, OpcodeGroup::kVector, "lvx128", 4, 0, true},
    {Opcode::stvx128, InstrFormat::kX, OpcodeGroup::kVector, "stvx128", 4, 0, true},
    {Opcode::lvlx128, InstrFormat::kX, OpcodeGroup::kVector, "lvlx128", 4, 0, true},
    {Opcode::lvrx128, InstrFormat::kX, OpcodeGroup::kVector, "lvrx128", 4, 0, true},
    {Opcode::stvlx128, InstrFormat::kX, OpcodeGroup::kVector, "stvlx128", 4, 0, true},
    {Opcode::stvrx128, InstrFormat::kX, OpcodeGroup::kVector, "stvrx128", 4, 0, true},
    {Opcode::lvlxl128, InstrFormat::kX, OpcodeGroup::kVector, "lvlxl128", 4, 0, true},
    {Opcode::lvrxl128, InstrFormat::kX, OpcodeGroup::kVector, "lvrxl128", 4, 0, true},
    {Opcode::stvlxl128, InstrFormat::kX, OpcodeGroup::kVector, "stvlxl128", 4, 0, true},
    {Opcode::stvrxl128, InstrFormat::kX, OpcodeGroup::kVector, "stvrxl128", 4, 0, true},
    {Opcode::vmulfp128, InstrFormat::kX, OpcodeGroup::kVector, "vmulfp128", 4, 0, true},
    {Opcode::vdot3fp128, InstrFormat::kX, OpcodeGroup::kVector, "vdot3fp128", 4, 0, true},
    {Opcode::vdot4fp128, InstrFormat::kX, OpcodeGroup::kVector, "vdot4fp128", 4, 0, true},
    {Opcode::vmsum3fp128, InstrFormat::kX, OpcodeGroup::kVector, "vmsum3fp128", 4, 0, true},
    {Opcode::vmsum4fp128, InstrFormat::kX, OpcodeGroup::kVector, "vmsum4fp128", 4, 0, true},
    {Opcode::vperm128, InstrFormat::kX, OpcodeGroup::kVector, "vperm128", 4, 0, true},
    {Opcode::vmrgow128, InstrFormat::kX, OpcodeGroup::kVector, "vmrgow128", 4, 0, true},
    {Opcode::vmrgew128, InstrFormat::kX, OpcodeGroup::kVector, "vmrgew128", 4, 0, true},
    {Opcode::vrlw128, InstrFormat::kX, OpcodeGroup::kVector, "vrlw128", 4, 0, true},
    {Opcode::vslw128, InstrFormat::kX, OpcodeGroup::kVector, "vslw128", 4, 0, true},
    {Opcode::vsrw128, InstrFormat::kX, OpcodeGroup::kVector, "vsrw128", 4, 0, true},
    {Opcode::vsraw128, InstrFormat::kX, OpcodeGroup::kVector, "vsraw128", 4, 0, true},
    {Opcode::vupkd3d128, InstrFormat::kX, OpcodeGroup::kVector, "vupkd3d128", 4, 0, true},
    {Opcode::vpkd3d128, InstrFormat::kX, OpcodeGroup::kVector, "vpkd3d128", 4, 0, true},
    {Opcode::vorc, InstrFormat::kX, OpcodeGroup::kVector, "vorc", 4, 0, true},

    //=========================================================================
    // Additional Integer Operations
    //=========================================================================
    {Opcode::mulli, InstrFormat::kD, OpcodeGroup::kGeneral, "mulli", 7, 0, false},
    {Opcode::subfic, InstrFormat::kD, OpcodeGroup::kGeneral, "subfic", 8, 0, false},
    {Opcode::addic, InstrFormat::kD, OpcodeGroup::kGeneral, "addic", 12, 0, false},
    {Opcode::addic_, InstrFormat::kD, OpcodeGroup::kGeneral, "addic.", 13, 0, false},
    {Opcode::lha, InstrFormat::kD, OpcodeGroup::kMemory, "lha", 42, 0, false},
    {Opcode::mullw, InstrFormat::kXO, OpcodeGroup::kGeneral, "mullw", 31, 235, true},
    {Opcode::mulhw, InstrFormat::kXO, OpcodeGroup::kGeneral, "mulhw", 31, 75, true},
    {Opcode::mulhwu, InstrFormat::kXO, OpcodeGroup::kGeneral, "mulhwu", 31, 11, true},
    {Opcode::divw, InstrFormat::kXO, OpcodeGroup::kGeneral, "divw", 31, 491, true},
    {Opcode::divwu, InstrFormat::kXO, OpcodeGroup::kGeneral, "divwu", 31, 459, true},
    {Opcode::cntlzw, InstrFormat::kX, OpcodeGroup::kGeneral, "cntlzw", 31, 26, true},
    {Opcode::srawi, InstrFormat::kX, OpcodeGroup::kGeneral, "srawi", 31, 824, true},
    {Opcode::extsb, InstrFormat::kX, OpcodeGroup::kGeneral, "extsb", 31, 954, true},
    {Opcode::extsh, InstrFormat::kX, OpcodeGroup::kGeneral, "extsh", 31, 922, true},

    //=========================================================================
    // Indexed Memory Operations
    //=========================================================================
    {Opcode::lbzx, InstrFormat::kX, OpcodeGroup::kMemory, "lbzx", 31, 87, true},
    {Opcode::lhzx, InstrFormat::kX, OpcodeGroup::kMemory, "lhzx", 31, 279, true},
    {Opcode::lhax, InstrFormat::kX, OpcodeGroup::kMemory, "lhax", 31, 311, true},
    {Opcode::lwzx, InstrFormat::kX, OpcodeGroup::kMemory, "lwzx", 31, 23, true},
    {Opcode::ldx, InstrFormat::kX, OpcodeGroup::kMemory, "ldx", 31, 21, true},
    {Opcode::stbx, InstrFormat::kX, OpcodeGroup::kMemory, "stbx", 31, 215, true},
    {Opcode::sthx, InstrFormat::kX, OpcodeGroup::kMemory, "sthx", 31, 407, true},
    {Opcode::stwx, InstrFormat::kX, OpcodeGroup::kMemory, "stwx", 31, 151, true},
    {Opcode::stdx, InstrFormat::kX, OpcodeGroup::kMemory, "stdx", 31, 149, true},
}};

// Map for fast lookup: (primary_opcode, extended_opcode) -> index
static std::unordered_map<u64, size_t> g_opcode_lookup_map;
static std::once_flag g_opcode_init_flag;

// Initialize lookup map implementation
static void init_opcode_lookup_map_impl() {
  g_opcode_lookup_map.reserve(g_opcode_table.size());
  for (size_t i = 0; i < g_opcode_table.size(); ++i) {
    const auto& info = g_opcode_table[i];
    if (info.opcode == Opcode::kUnknown)
      continue;

    u64 key = ((u64)info.primary_opcode << 32) | info.extended_opcode;
    g_opcode_lookup_map[key] = i;
  }
}

// Thread-safe initialization (called once)
static void init_opcode_lookup_map() {
  std::call_once(g_opcode_init_flag, init_opcode_lookup_map_impl);
}

//=============================================================================
// Public API
//=============================================================================

Opcode lookup_opcode(u32 code) {
  init_opcode_lookup_map();

  // Extract primary opcode (bits 0-5)
  u32 primary = extract_bits(code, 0, 6);

  // Handle instructions with no extended opcode
  switch (primary) {
    case 3:
      return Opcode::twi;
    case 7:
      return Opcode::mulli;
    case 8:
      return Opcode::subfic;
    case 10:
      return Opcode::cmpli;
    case 11:
      return Opcode::cmpi;
    case 12:
      return Opcode::addic;
    case 13:
      return Opcode::addic_;
    case 14:
      return Opcode::addi;
    case 15:
      return Opcode::addis;
    case 16: {
      // Conditional branch - check AA (bit 30) and LK (bit 31)
      // Note: PPC bit numbering is MSB=0, so bit 30 is position 1, bit 31 is position 0
      bool aa = (code >> 1) & 1;  // Absolute address
      bool lk = code & 1;         // Link
      if (lk && aa)
        return Opcode::bcla;
      if (lk)
        return Opcode::bcl;
      if (aa)
        return Opcode::bca;
      return Opcode::bc;
    }
    case 17:
      return Opcode::sc;
    case 18: {
      // Unconditional branch - check AA (bit 30) and LK (bit 31)
      bool aa = (code >> 1) & 1;  // Absolute address
      bool lk = code & 1;         // Link
      if (lk && aa)
        return Opcode::bla;
      if (lk)
        return Opcode::bl;
      if (aa)
        return Opcode::ba;
      return Opcode::b;
    }
    case 21:
      return Opcode::rlwinm;
    case 23:
      return Opcode::rlwnm;
    case 24:
      return Opcode::ori;
    case 25:
      return Opcode::oris;
    case 26:
      return Opcode::xori;
    case 27:
      return Opcode::xoris;
    case 28:
      return Opcode::andi_;
    case 29:
      return Opcode::andis_;
    case 32:
      return Opcode::lwz;
    case 33:
      return Opcode::lwzu;
    case 34:
      return Opcode::lbz;
    case 35:
      return Opcode::lbzu;
    case 36:
      return Opcode::stw;
    case 37:
      return Opcode::stwu;
    case 38:
      return Opcode::stb;
    case 39:
      return Opcode::stbu;
    case 40:
      return Opcode::lhz;
    case 41:
      return Opcode::lhzu;
    case 42:
      return Opcode::lha;
    case 44:
      return Opcode::sth;
    case 45:
      return Opcode::sthu;

      // Floating-point load/store
    case 48:
      return Opcode::lfs;
    case 49:
      return Opcode::lfsu;
    case 50:
      return Opcode::lfd;
    case 51:
      return Opcode::lfdu;
    case 52:
      return Opcode::stfs;
    case 53:
      return Opcode::stfsu;
    case 54:
      return Opcode::stfd;
    case 55:
      return Opcode::stfdu;
  }

  // Handle instructions with extended opcodes
  u32 extended = 0;
  if (primary == 19) {
    // XL format: extended opcode in bits 21-30
    extended = extract_bits(code, 21, 10);
    bool lk = code & 1;  // LK bit is bit 31 (position 0)
    if (extended == 16)
      return lk ? Opcode::bclrl : Opcode::bclr;
    if (extended == 528)
      return lk ? Opcode::bcctrl : Opcode::bcctr;
    if (extended == 150)
      return Opcode::isync;
  } else if (primary == 31) {
    // X/XO/XFX format: extended opcode in bits 21-30
    extended = extract_bits(code, 21, 10);

    // Map extended opcodes for primary 31
    switch (extended) {
      case 0:
        return Opcode::cmp;
      case 4:
        return Opcode::tw;
      case 11:
        return Opcode::mulhwu;
      case 19:
        return Opcode::mfcr;
      case 23:
        return Opcode::lwzx;
      case 24:
        return Opcode::slw;
      case 26:
        return Opcode::cntlzw;
      case 28:
        return Opcode::and_;
      case 32:
        return Opcode::cmpl;
      case 40:
        return Opcode::subf;
      case 60:
        return Opcode::andc;
      case 75:
        return Opcode::mulhw;
      case 87:
        return Opcode::lbzx;
      case 104:
        return Opcode::neg;
      case 124:
        return Opcode::nor;
      case 144:
        return Opcode::mtcr;
      case 151:
        return Opcode::stwx;
      case 215:
        return Opcode::stbx;
      case 235:
        return Opcode::mullw;
      case 266:
        return Opcode::add;
      case 279:
        return Opcode::lhzx;
      case 284:
        return Opcode::eqv;
      case 311:
        return Opcode::lhax;
      case 316:
        return Opcode::xor_;
      case 339:
        return Opcode::mfspr;
      case 407:
        return Opcode::sthx;
      case 412:
        return Opcode::orc;
      case 444:
        return Opcode::or_;
      case 459:
        return Opcode::divwu;
      case 467:
        return Opcode::mtspr;
      case 476:
        return Opcode::nand;
      case 491:
        return Opcode::divw;
      case 536:
        return Opcode::srw;
      case 598:
        return Opcode::sync;
      case 792:
        return Opcode::sraw;
      case 824:
        return Opcode::srawi;
      case 922:
        return Opcode::extsh;
      case 954:
        return Opcode::extsb;
    }
  } else if (primary == 58) {
    // DS format: XO in bits 30-31
    extended = extract_bits(code, 30, 2);
    if (extended == 0)
      return Opcode::ld;
    if (extended == 1)
      return Opcode::ldu;
  } else if (primary == 62) {
    // DS format: XO in bits 30-31
    extended = extract_bits(code, 30, 2);
    if (extended == 0)
      return Opcode::std;
    if (extended == 1)
      return Opcode::stdu;
  } else if (primary == 59) {
    // Single-precision floating-point (A-form): extended in bits 26-30
    extended = extract_bits(code, 26, 5);
    switch (extended) {
      case 18:
        return Opcode::fdivs;
      case 20:
        return Opcode::fsubs;
      case 21:
        return Opcode::fadds;
      case 22:
        return Opcode::fsqrts;
      case 24:
        return Opcode::fres;
      case 25:
        return Opcode::fmuls;
      case 26:
        return Opcode::frsqrtes;
      case 28:
        return Opcode::fmsubs;
      case 29:
        return Opcode::fmadds;
      case 30:
        return Opcode::fnmsubs;
      case 31:
        return Opcode::fnmadds;
    }
  } else if (primary == 63) {
    // Double-precision floating-point: check both X-form and A-form
    // First try X-form extended opcode (bits 21-30)
    extended = extract_bits(code, 21, 10);
    switch (extended) {
      case 0:
        return Opcode::fcmpu;
      case 12:
        return Opcode::frsp;
      case 14:
        return Opcode::fctiw;
      case 15:
        return Opcode::fctiwz;
      case 32:
        return Opcode::fcmpo;
      case 40:
        return Opcode::fneg;
      case 72:
        return Opcode::fmr;
      case 136:
        return Opcode::fnabs;
      case 264:
        return Opcode::fabs;
      case 583:
        return Opcode::mffs;
      case 711:
        return Opcode::mtfsf;
      case 814:
        return Opcode::fctid;
      case 815:
        return Opcode::fctidz;
      case 846:
        return Opcode::fcfid;
    }
    // Try A-form extended opcode (bits 26-30)
    extended = extract_bits(code, 26, 5);
    switch (extended) {
      case 18:
        return Opcode::fdiv;
      case 20:
        return Opcode::fsub;
      case 21:
        return Opcode::fadd;
      case 22:
        return Opcode::fsqrt;
      case 23:
        return Opcode::fsel;
      case 24:
        return Opcode::fre;
      case 25:
        return Opcode::fmul;
      case 26:
        return Opcode::frsqrte;
      case 28:
        return Opcode::fmsub;
      case 29:
        return Opcode::fmadd;
      case 30:
        return Opcode::fnmsub;
      case 31:
        return Opcode::fnmadd;
    }
  } else if (primary == 4) {
    // VMX/AltiVec instructions (primary opcode 4)
    // Try VA-form first (bits 26-31, 6-bit XO)
    extended = extract_bits(code, 26, 6);
    switch (extended) {
      case 32:
        return Opcode::vmaddfp;  // vmaddfp vD, vA, vC, vB
      case 33:
        return Opcode::vnmsubfp;  // vnmsubfp vD, vA, vC, vB
      case 43:
        return Opcode::vperm;  // vperm vD, vA, vB, vC
      case 44:
        return Opcode::vsel;  // vsel vD, vA, vB, vC
    }

    // Try VX-form (bits 21-31, 11-bit XO)
    extended = extract_bits(code, 21, 11);
    switch (extended) {
        // Vector load/store
      case 7:
        return Opcode::lvx;  // lvx vD, rA, rB (XO=7 is lvebx, lvx is 103)
      case 39:
        return Opcode::lvlx;  // lvlx vD, rA, rB
      case 71:
        return Opcode::lvrx;  // lvrx vD, rA, rB
      case 103:
        return Opcode::lvx;  // lvx vD, rA, rB
      case 135:
        return Opcode::stvx;  // stvx vS, rA, rB
      case 167:
        return Opcode::stvlx;  // stvlx vS, rA, rB
      case 199:
        return Opcode::stvrx;  // stvrx vS, rA, rB
      case 231:
        return Opcode::stvx;  // stvx vS, rA, rB
      case 359:
        return Opcode::lvxl;  // lvxl vD, rA, rB
      case 487:
        return Opcode::stvxl;  // stvxl vS, rA, rB
      case 6:
        return Opcode::lvsl;  // lvsl vD, rA, rB
      case 38:
        return Opcode::lvsr;  // lvsr vD, rA, rB

        // Vector floating-point arithmetic
      case 10:
        return Opcode::vaddfp;  // vaddfp vD, vA, vB
      case 74:
        return Opcode::vsubfp;  // vsubfp vD, vA, vB
      case 1034:
        return Opcode::vmaxfp;  // vmaxfp vD, vA, vB
      case 1098:
        return Opcode::vminfp;  // vminfp vD, vA, vB
      case 266:
        return Opcode::vrsqrtefp;  // vrsqrtefp vD, vB
      case 330:
        return Opcode::vrefp;  // vrefp vD, vB
      case 394:
        return Opcode::vlogfp;  // vlogfp vD, vB
      case 458:
        return Opcode::vexptefp;  // vexptefp vD, vB

        // Vector integer arithmetic
      case 0:
        return Opcode::vaddubm;  // vaddubm vD, vA, vB
      case 64:
        return Opcode::vadduhm;  // vadduhm vD, vA, vB
      case 128:
        return Opcode::vadduwm;  // vadduwm vD, vA, vB
      case 1024:
        return Opcode::vsububm;  // vsububm vD, vA, vB
      case 1088:
        return Opcode::vsubuhm;  // vsubuhm vD, vA, vB
      case 1152:
        return Opcode::vsubuwm;  // vsubuwm vD, vA, vB
      case 8:
        return Opcode::vmuloub;  // vmuloub vD, vA, vB
      case 72:
        return Opcode::vmulouh;  // vmulouh vD, vA, vB
      case 264:
        return Opcode::vmuleub;  // vmuleub vD, vA, vB
      case 328:
        return Opcode::vmuleuh;  // vmuleuh vD, vA, vB
      case 1026:
        return Opcode::vavgub;  // vavgub vD, vA, vB
      case 1090:
        return Opcode::vavguh;  // vavguh vD, vA, vB
      case 1154:
        return Opcode::vavguw;  // vavguw vD, vA, vB

        // Vector logical
      case 1028:
        return Opcode::vand;  // vand vD, vA, vB
      case 1092:
        return Opcode::vandc;  // vandc vD, vA, vB
      case 1156:
        return Opcode::vor;  // vor vD, vA, vB
      case 1220:
        return Opcode::vxor;  // vxor vD, vA, vB
      case 1284:
        return Opcode::vnor;  // vnor vD, vA, vB

        // Vector merge
      case 12:
        return Opcode::vmrghb;  // vmrghb vD, vA, vB
      case 76:
        return Opcode::vmrghh;  // vmrghh vD, vA, vB
      case 140:
        return Opcode::vmrghw;  // vmrghw vD, vA, vB
      case 268:
        return Opcode::vmrglb;  // vmrglb vD, vA, vB
      case 332:
        return Opcode::vmrglh;  // vmrglh vD, vA, vB
      case 396:
        return Opcode::vmrglw;  // vmrglw vD, vA, vB

        // Vector pack/unpack
      case 14:
        return Opcode::vpkuhum;  // vpkuhum vD, vA, vB
      case 78:
        return Opcode::vpkuwum;  // vpkuwum vD, vA, vB
      case 142:
        return Opcode::vpkuhus;  // vpkuhus vD, vA, vB
      case 206:
        return Opcode::vpkuwus;  // vpkuwus vD, vA, vB
      case 270:
        return Opcode::vpkshus;  // vpkshus vD, vA, vB
      case 334:
        return Opcode::vpkswus;  // vpkswus vD, vA, vB
      case 398:
        return Opcode::vpkshss;  // vpkshss vD, vA, vB
      case 462:
        return Opcode::vpkswss;  // vpkswss vD, vA, vB
      case 526:
        return Opcode::vupkhsb;  // vupkhsb vD, vB
      case 590:
        return Opcode::vupkhsh;  // vupkhsh vD, vB
      case 654:
        return Opcode::vupklsb;  // vupklsb vD, vB
      case 718:
        return Opcode::vupklsh;  // vupklsh vD, vB

        // Vector splat
      case 524:
        return Opcode::vspltb;  // vspltb vD, vB, UIMM
      case 588:
        return Opcode::vsplth;  // vsplth vD, vB, UIMM
      case 652:
        return Opcode::vspltw;  // vspltw vD, vB, UIMM
      case 780:
        return Opcode::vspltisb;  // vspltisb vD, SIMM
      case 844:
        return Opcode::vspltish;  // vspltish vD, SIMM
      case 908:
        return Opcode::vspltisw;  // vspltisw vD, SIMM

        // Vector shift/rotate
      case 260:
        return Opcode::vslb;  // vslb vD, vA, vB
      case 324:
        return Opcode::vslh;  // vslh vD, vA, vB
      case 388:
        return Opcode::vslw;  // vslw vD, vA, vB
      case 516:
        return Opcode::vsrb;  // vsrb vD, vA, vB
      case 580:
        return Opcode::vsrh;  // vsrh vD, vA, vB
      case 644:
        return Opcode::vsrw;  // vsrw vD, vA, vB
      case 772:
        return Opcode::vsrab;  // vsrab vD, vA, vB
      case 836:
        return Opcode::vsrah;  // vsrah vD, vA, vB
      case 900:
        return Opcode::vsraw;  // vsraw vD, vA, vB
      case 4:
        return Opcode::vrlb;  // vrlb vD, vA, vB
      case 68:
        return Opcode::vrlh;  // vrlh vD, vA, vB
      case 132:
        return Opcode::vrlw;  // vrlw vD, vA, vB
      case 452:
        return Opcode::vsl;  // vsl vD, vA, vB
      case 708:
        return Opcode::vsr;  // vsr vD, vA, vB
      case 1036:
        return Opcode::vslo;  // vslo vD, vA, vB
      case 1100:
        return Opcode::vsro;  // vsro vD, vA, vB

        // Vector conversion
      case 778:
        return Opcode::vcfux;  // vcfux vD, vB, UIMM
      case 842:
        return Opcode::vcfsx;  // vcfsx vD, vB, UIMM
      case 906:
        return Opcode::vctuxs;  // vctuxs vD, vB, UIMM
      case 970:
        return Opcode::vctsxs;  // vctsxs vD, vB, UIMM
      case 522:
        return Opcode::vrfin;  // vrfin vD, vB
      case 586:
        return Opcode::vrfiz;  // vrfiz vD, vB
      case 650:
        return Opcode::vrfip;  // vrfip vD, vB
      case 714:
        return Opcode::vrfim;  // vrfim vD, vB

        // Vector status/control
      case 1540:
        return Opcode::mfvscr;  // mfvscr vD
      case 1604:
        return Opcode::mtvscr;  // mtvscr vB
    }

    // Try VXR-form for compare instructions (bits 21-30, 10-bit XO with Rc at 31)
    extended = extract_bits(code, 21, 10);
    u32 rc = extract_bits(code, 31, 1);
    switch (extended) {
      case 198:
        return rc ? Opcode::vcmpeqfp_ : Opcode::vcmpeqfp;
      case 454:
        return rc ? Opcode::vcmpgefp_ : Opcode::vcmpgefp;
      case 710:
        return rc ? Opcode::vcmpgtfp_ : Opcode::vcmpgtfp;
      case 966:
        return Opcode::vcmpbfp;
      case 6:
        return Opcode::vcmpequb;
      case 70:
        return Opcode::vcmpequh;
      case 134:
        return Opcode::vcmpequw;
      case 518:
        return Opcode::vcmpgtub;
      case 582:
        return Opcode::vcmpgtuh;
      case 646:
        return Opcode::vcmpgtuw;
      case 774:
        return Opcode::vcmpgtsb;
      case 838:
        return Opcode::vcmpgtsh;
      case 902:
        return Opcode::vcmpgtsw;
    }

    // VMX128 Load/Store instructions (Xbox 360 extension)
    // These use primary opcode 4 with bits 30-31 = 11 (value 3)
    u32 bits_30_31 = code & 0x3;  // Extract bits 30-31
    if (bits_30_31 == 3) {
      // VMX128 load/store format: bits 21-27 contain extended opcode
      u32 vmx128_xo = extract_bits(code, 21, 7);
      switch (vmx128_xo) {
        case 0:
          return Opcode::lvsl128;  // 0000000
        case 4:
          return Opcode::lvsr128;  // 0000100
        case 8:
          return Opcode::lvewx128;  // 0001000
        case 12:
          return Opcode::lvx128;  // 0001100
        case 28:
          return Opcode::stvx128;  // 0011100
        case 44:
          return Opcode::lvxl128;  // 0101100
        case 48:
          return Opcode::stvewx128;  // 0110000
        case 60:
          return Opcode::stvxl128;  // 0111100
        case 64:
          return Opcode::lvlx128;  // 1000000
        case 68:
          return Opcode::lvrx128;  // 1000100
        case 80:
          return Opcode::stvlx128;  // 1010000
        case 84:
          return Opcode::stvrx128;  // 1010100
        case 96:
          return Opcode::lvlxl128;  // 1100000
        case 100:
          return Opcode::lvrxl128;  // 1100100
        case 112:
          return Opcode::stvlxl128;  // 1110000
        case 116:
          return Opcode::stvrxl128;  // 1110100
      }
      // vsldoi128 has a different pattern: bit 27=1, bits 22-26 encode SHB
      // |0 0 0 1 0 0|VD128|VA128|VB128|A|SHB|a|1|VDh|VBh|
      if ((code & 0x10) == 0x10) {  // bit 27 == 1
        return Opcode::vsldoi128;
      }
    }
  }

  // VMX128 arithmetic/logical instructions (primary opcode 5)
  // Format: |0 0 0 1 0 1|VD128|VA128|VB128|A|xxxx|a|y|VDh|VBh|
  else if (primary == 5) {
    // Extract operation bits 22-25 (4 bits) and bit 27
    u32 op4 = extract_bits(code, 22, 4);
    u32 bit27 = extract_bits(code, 27, 1);
    u32 bit26 = extract_bits(code, 26, 1);

    if (bit27 == 1) {
      // Most VMX128 arithmetic instructions
      switch (op4) {
        case 0:
          return Opcode::vaddfp128;  // 0000
        case 1:
          return bit26 ? Opcode::vsubfp128 : Opcode::vrlw128;  // 0001
        case 2:
          return Opcode::vmulfp128;  // 0010
        case 3:
          return Opcode::vmaddfp128;  // 0011
        case 4:
          return Opcode::vmaddcfp128;  // 0100
        case 5:
          return Opcode::vnmsubfp128;  // 0101
        case 6:
          return Opcode::vmsum3fp128;  // 0110
        case 7:
          return Opcode::vmsum4fp128;  // 0111
        case 8:
          return Opcode::vand128;  // 1000
        case 9:
          return Opcode::vpkshss128;  // 1001 (bit27=0) or other
        case 10:
          return bit26 ? Opcode::vnor128 : Opcode::vandc128;  // 1010
        case 11:
          return bit26 ? Opcode::vor128 : Opcode::vpkswss128;  // 1011
        case 12:
          return Opcode::vxor128;  // 1100
        case 13:
          return Opcode::vsel128;  // 1101
        case 14:
          return Opcode::vslo128;  // 1110
        case 15:
          return Opcode::vsro128;  // 1111 (only in primary 6?)
      }
    } else {
      // VMX128 instructions with bit27=0
      switch (op4) {
        case 0:
          return Opcode::vperm128;  // vperm has different encoding
        case 8:
          return Opcode::vpkshss128;
        case 9:
          return Opcode::vpkshus128;
        case 10:
          return Opcode::vpkswss128;
        case 11:
          return Opcode::vpkswus128;
        case 12:
          return Opcode::vpkuhum128;
        case 13:
          return Opcode::vpkuhus128;
        case 14:
          return Opcode::vpkuwum128;
        case 15:
          return Opcode::vpkuwus128;
      }
    }
  }

  // VMX128 compare/convert/unary instructions (primary opcode 6)
  // Format varies by instruction
  else if (primary == 6) {
    u32 op4 = extract_bits(code, 22, 4);
    u32 bit27 = extract_bits(code, 27, 1);
    [[maybe_unused]] u32 bit26 = extract_bits(code, 26, 1);
    u32 bits_21_27 = extract_bits(code, 21, 7);

    // Check for specific extended opcodes
    if (bit27 == 0) {
      // Compare and some other instructions
      switch (op4) {
        case 0:
          return Opcode::vcmpeqfp128;  // 0000
        case 1:
          return Opcode::vcmpgefp128;  // 0001
        case 2:
          return Opcode::vcmpgtfp128;  // 0010
        case 3:
          return Opcode::vcmpbfp128;  // 0011
        case 8:
          return Opcode::vcmpequw128;  // 1000
        case 10:
          return Opcode::vmaxfp128;  // 1010
        case 11:
          return Opcode::vminfp128;  // 1011
        case 12:
          return Opcode::vmrghw128;  // 1100
        case 13:
          return Opcode::vmrglw128;  // 1101
      }
    } else {
      // bit27 == 1: conversion, rounding, splat, shift instructions
      switch (bits_21_27) {
        case 0x23:
          return Opcode::vcfpsxws128;  // 0100011
        case 0x27:
          return Opcode::vcfpuxws128;  // 0100111
        case 0x2B:
          return Opcode::vcsxwfp128;  // 0101011
        case 0x2F:
          return Opcode::vcuxwfp128;  // 0101111
        case 0x33:
          return Opcode::vrfim128;  // 0110011
        case 0x37:
          return Opcode::vrfin128;  // 0110111
        case 0x3B:
          return Opcode::vrfip128;  // 0111011
        case 0x3F:
          return Opcode::vrfiz128;  // 0111111
        case 0x63:
          return Opcode::vrefp128;  // 1100011
        case 0x67:
          return Opcode::vrsqrtefp128;  // 1100111
        case 0x6B:
          return Opcode::vexptefp128;  // 1101011
        case 0x6F:
          return Opcode::vlogefp128;  // 1101111
        case 0x73:
          return Opcode::vspltw128;  // 1110011
        case 0x77:
          return Opcode::vspltisw128;  // 1110111
        case 0x7F:
          return Opcode::vupkd3d128;  // 1111111
      }
      // vpermwi128, vpkd3d128, vrlimi128 have special encodings
      u32 bits_25_27 = extract_bits(code, 25, 3);
      if (bits_25_27 == 1) {  // 001
        return Opcode::vpermwi128;
      }
      if (bits_25_27 == 5) {  // 101
        return Opcode::vrlimi128;
      }
      u32 bits_23_25 = extract_bits(code, 23, 3);
      if (extract_bits(code, 21, 2) == 3 && (bits_23_25 >= 4)) {
        return Opcode::vpkd3d128;
      }
      // vslw128, vsrw128, vsraw128, vsro128
      if (op4 == 3)
        return Opcode::vslw128;
      if (op4 == 5)
        return Opcode::vsraw128;
      if (op4 == 7)
        return Opcode::vsrw128;
      if (op4 == 15)
        return Opcode::vsro128;
    }
  }

  return Opcode::kUnknown;
}

const OpcodeInfo& get_opcode_info(Opcode opcode) {
  init_opcode_lookup_map();

  // Static unknown entry for fallback
  static const OpcodeInfo unknown = {
      Opcode::kUnknown, InstrFormat::kUnknown, OpcodeGroup::kGeneral, "unknown", 0, 0, false};

  // bail early if we already know its unknown.
  if (opcode == Opcode::kUnknown) {
    return unknown;
  }

  // Linear search through table (small table, acceptable)
  for (const auto& info : g_opcode_table) {
    // Skip uninitialized entries (name will be null)
    if (info.name == nullptr)
      continue;
    if (info.opcode == opcode) {
      return info;
    }
  }
  return unknown;
}

}  // namespace rex::codegen::ppc
