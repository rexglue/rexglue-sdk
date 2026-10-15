/**
 * @file        codegen/builders.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <ppc-inst.h>

namespace rex::codegen {

struct BuilderContext;

/**
 * Builds the C++ code of a PPC instruction. Each opcode with a builder has an explicit
 * specialization.
 *
 * @tparam Opcode The PPC instruction ID (PPC_INST_*).
 * @param ctx The builder context.
 * @return True when the instruction was handled.
 */
template <int Opcode>
bool Build(BuilderContext& ctx);

/**
 * Build C++ code for a PPC instruction using the dispatch table.
 *
 * @param id The PPC instruction ID (PPC_INST_*)
 * @param ctx The builder context
 * @return true if instruction was handled, false if unknown
 */
bool DispatchInstruction(int id, BuilderContext& ctx);

template <>
bool Build<PPC_INST_CMPD>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_CMPDI>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_CMPLD>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_CMPLDI>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_CMPLW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_CMPLWI>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_CMPW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_CMPWI>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_ADD>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_ADDC>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_ADDE>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_ADDI>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_ADDIC>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_ADDIS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_ADDME>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_ADDZE>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_DIVD>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_DIVDU>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_DIVW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_DIVWU>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_MULHD>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MULHDU>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MULHW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MULHWU>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MULLD>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MULLI>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MULLW>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_NEG>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_SUBF>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_SUBFC>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_SUBFE>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_SUBFIC>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_SUBFME>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_SUBFZE>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_AND>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_ANDC>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_ANDI>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_ANDIS>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_NAND>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_NOR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_NOT>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_OR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_ORC>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_ORI>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_ORIS>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_XOR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_XORI>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_XORIS>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_CRAND>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_CRANDC>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_CREQV>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_CRNAND>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_CRNOR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_CROR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_CRORC>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_CRXOR>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_EQV>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_CNTLZD>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_CNTLZW>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_EXTSB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_EXTSH>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_EXTSW>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_CLRLWI>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_RLDCL>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_RLDCR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_RLDIC>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_RLDICL>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_RLDICR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_RLDIMI>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_ROTLDI>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_RLWIMI>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_RLWINM>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_RLWNM>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_ROTLW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_ROTLWI>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_SLD>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_SLW>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_SRAD>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_SRADI>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_SRAW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_SRAWI>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_SRD>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_SRW>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_B>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BL>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BLR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BLRL>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_BCTR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BCTRL>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BNECTR>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_BDZ>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BDZF>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BDZLR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BDNZ>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BDNZF>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BDNZLR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BDNZT>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_BEQ>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BEQLR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BNE>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BNELR>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_BLT>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BLTLR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BGE>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BGELR>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_BGT>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BGTLR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BLE>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BLELR>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_BSO>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BSOLR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BNS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_BNSLR>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_FABS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FNABS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FNEG>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_FMR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FCFID>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FCTID>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FCTIDZ>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FCTIW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FCTIWZ>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FRSP>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_FCMPU>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FCMPO>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_FADD>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FADDS>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_FSUB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FSUBS>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_FMUL>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FMULS>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_FDIV>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FDIVS>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_FMADD>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FMADDS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FMSUB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FMSUBS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FNMADD>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FNMADDS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FNMSUB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FNMSUBS>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_FRES>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FRSQRTE>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FSQRT>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_FSQRTS>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_FSEL>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_LI>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LIS>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_LBZ>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LBZU>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LBZX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LBZUX>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_LHA>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LHAU>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LHAUX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LHAX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LHBRX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LHZ>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LHZU>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LHZUX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LHZX>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_LWA>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LWAUX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LWAX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LWBRX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LWZ>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LWZU>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LWZUX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LWZX>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_LD>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LDU>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LDX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LDUX>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_LWARX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LDARX>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_LFD>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LFDU>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LFDUX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LFDX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LFS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LFSU>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LFSUX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LFSX>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_STB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STBU>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STBX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STBUX>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_STH>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STHBRX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STHU>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STHUX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STHX>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_STW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STWU>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STWUX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STWX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STWBRX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STMW>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_STWCX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STDCX>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_STD>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STDU>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STDX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STDUX>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_STFD>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STFDU>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STFDUX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STFDX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STFIWX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STFS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STFSU>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STFSUX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STFSX>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_LVX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LVLX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LVRX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LVSL>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LVSR>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_STVEBX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STVEHX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STVEWX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STVLX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STVRX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_STVX>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_NOP>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_ATTN>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_SYNC>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_LWSYNC>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_EIEIO>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_DB16CYC>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_CCTPL>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_CCTPM>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_CCTPH>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_TWI>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_TDI>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_TW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_TD>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_DCBF>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_DCBT>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_DCBTST>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_DCBZ>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_DCBZL>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_DCBST>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_MR>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_MCRF>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_MFCTR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MFCR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MFXER>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MFOCRF>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MFLR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MFMSR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MFFS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MFTB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MFTBU>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_MTCR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MTCRF>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MTCTR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MTLR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MTMSRD>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MTFSF>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MTXER>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_CLRLDI>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_VADDFP>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSUBFP>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMULFP128>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMADDFP>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VNMSUBFP>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMAXFP>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMINFP>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VREFP>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VRSQRTEFP>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VEXPTEFP>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VLOGEFP>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_VMSUM3FP128>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMSUM4FP128>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_VRFIM>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VRFIN>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VRFIP>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VRFIZ>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_VADDSBS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VADDSHS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VADDSWS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VADDUBM>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VADDUBS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VADDUHM>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VADDUWM>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VADDUWS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VADDUHS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSUBSBS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSUBSHS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSUBSWS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSUBUBM>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSUBUBS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSUBUWS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSUBUHS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSUBUHM>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSUBUWM>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMAXSH>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMAXSB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMAXSW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMAXUH>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMINSH>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMINSB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMINSW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMINUH>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMINUW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMAXUB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMINUB>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_VAVGSB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VAVGSH>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VAVGSW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VAVGUB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VAVGUH>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_VAND>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VANDC>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VANDC128>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VOR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VXOR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VNOR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSEL>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_VCMPBFP>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VCMPEQFP>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VCMPEQUB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VCMPEQUH>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VCMPEQUW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VCMPGEFP>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VCMPGTFP>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VCMPGTUB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VCMPGTUH>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VCMPGTUW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VCMPGTSB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VCMPGTSH>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VCMPGTSW>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_MTVSCR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_MFVSCR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VCTSXS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VCTUXS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VCFSX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VCFUX>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_VMRGHB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMRGHH>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMRGHW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMRGLB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMRGLH>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VMRGLW>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_VPERM>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VPERMWI128>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VRLIMI128>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_VSL>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSLB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSLH>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSLDOI>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSLW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSLO>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSR>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSRH>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSRB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSRAB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSRAH>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSRAW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSRW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSRO>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VRLH>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VRLW>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_VSPLTB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSPLTH>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSPLTISB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSPLTISH>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSPLTISW>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VSPLTW>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_VPKPX>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VPKUHUM>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VPKUHUS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VPKUWUM>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VPKUWUS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VPKSHSS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VPKSHUS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VPKSWSS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VPKSWUS>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VPKD3D128>(BuilderContext& ctx);

template <>
bool Build<PPC_INST_VUPKD3D128>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VUPKHSB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VUPKHSH>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VUPKLSB>(BuilderContext& ctx);
template <>
bool Build<PPC_INST_VUPKLSH>(BuilderContext& ctx);

}  // namespace rex::codegen
