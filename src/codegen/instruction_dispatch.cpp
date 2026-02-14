/**
 * @file        rexcodegen/instruction_dispatch.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builders.h"
#include "builder_context.h"
#include <rex/logging.h>
#include <ppc.h>
#include <ppc-inst.h>
#include <dis-asm.h>
#include <unordered_map>

namespace rex::codegen {

using Builder = bool (*)(BuilderContext&);

// Static dispatch table
static const std::unordered_map<int, Builder>& GetDispatchTable()
{
    static const std::unordered_map<int, Builder> table = {
        //=====================================================================
        // Arithmetic
        //=====================================================================
        { PPC_INST_ADD, build_add },
        { PPC_INST_ADDE, build_adde },
        { PPC_INST_ADDI, build_addi },
        { PPC_INST_ADDIC, build_addic },
        { PPC_INST_ADDIS, build_addis },
        { PPC_INST_ADDZE, build_addze },
        { PPC_INST_ADDME, build_addme },
        { PPC_INST_ADDC, build_addc },
        { PPC_INST_DIVD, build_divd },
        { PPC_INST_DIVDU, build_divdu },
        { PPC_INST_DIVW, build_divw },
        { PPC_INST_DIVWU, build_divwu },
        { PPC_INST_MULHW, build_mulhw },
        { PPC_INST_MULHWU, build_mulhwu },
        { PPC_INST_MULLD, build_mulld },
        { PPC_INST_MULLI, build_mulli },
        { PPC_INST_MULLW, build_mullw },
        { PPC_INST_NEG, build_neg },
        { PPC_INST_SUBF, build_subf },
        { PPC_INST_SUBFC, build_subfc },
        { PPC_INST_SUBFE, build_subfe },
        { PPC_INST_SUBFIC, build_subfic },
        { PPC_INST_SUBFZE, build_subfze },
        { PPC_INST_SUBFME, build_subfme },
        { PPC_INST_MULHD, build_mulhd },
        { PPC_INST_MULHDU, build_mulhdu },

        //=====================================================================
        // Logical
        //=====================================================================
        { PPC_INST_AND, build_and },
        { PPC_INST_ANDC, build_andc },
        { PPC_INST_ANDI, build_andi },
        { PPC_INST_ANDIS, build_andis },
        { PPC_INST_NAND, build_nand },
        { PPC_INST_NOR, build_nor },
        { PPC_INST_NOT, build_not },
        { PPC_INST_OR, build_or },
        { PPC_INST_ORC, build_orc },
        { PPC_INST_ORI, build_ori },
        { PPC_INST_ORIS, build_oris },
        { PPC_INST_XOR, build_xor },
        { PPC_INST_XORI, build_xori },
        { PPC_INST_XORIS, build_xoris },
        { PPC_INST_EQV, build_eqv },
        { PPC_INST_CNTLZD, build_cntlzd },
        { PPC_INST_CNTLZW, build_cntlzw },
        { PPC_INST_EXTSB, build_extsb },
        { PPC_INST_EXTSH, build_extsh },
        { PPC_INST_EXTSW, build_extsw },
        { PPC_INST_CLRLWI, build_clrlwi },
        { PPC_INST_RLDICL, build_rldicl },
        { PPC_INST_RLDICR, build_rldicr },
        { PPC_INST_RLDIMI, build_rldimi },
        { PPC_INST_ROTLDI, build_rotldi },
        { PPC_INST_RLWIMI, build_rlwimi },
        { PPC_INST_RLWINM, build_rlwinm },
        { PPC_INST_RLWNM, build_rlwnm },
        { PPC_INST_ROTLW, build_rotlw },
        { PPC_INST_ROTLWI, build_rotlwi },
        { PPC_INST_SLD, build_sld },
        { PPC_INST_SLW, build_slw },
        { PPC_INST_SRAD, build_srad },
        { PPC_INST_SRADI, build_sradi },
        { PPC_INST_SRAW, build_sraw },
        { PPC_INST_SRAWI, build_srawi },
        { PPC_INST_SRD, build_srd },
        { PPC_INST_SRW, build_srw },

        //=====================================================================
        // Comparison
        //=====================================================================
        { PPC_INST_CMPD, build_cmpd },
        { PPC_INST_CMPDI, build_cmpdi },
        { PPC_INST_CMPLD, build_cmpld },
        { PPC_INST_CMPLDI, build_cmpldi },
        { PPC_INST_CMPLW, build_cmplw },
        { PPC_INST_CMPLWI, build_cmplwi },
        { PPC_INST_CMPW, build_cmpw },
        { PPC_INST_CMPWI, build_cmpwi },

        //=====================================================================
        // Control Flow
        //=====================================================================
        { PPC_INST_B, build_b },
        { PPC_INST_BL, build_bl },
        { PPC_INST_BLR, build_blr },
        { PPC_INST_BLRL, build_blrl },
        { PPC_INST_BCTR, build_bctr },
        { PPC_INST_BCTRL, build_bctrl },
        { PPC_INST_BNECTR, build_bnectr },
        { PPC_INST_BDZ, build_bdz },
        { PPC_INST_BDZF, build_bdzf },
        { PPC_INST_BDZLR, build_bdzlr },
        { PPC_INST_BDNZ, build_bdnz },
        { PPC_INST_BDNZF, build_bdnzf },
        { PPC_INST_BDNZT, build_bdnzt },
        { PPC_INST_BEQ, build_beq },
        { PPC_INST_BEQLR, build_beqlr },
        { PPC_INST_BNE, build_bne },
        { PPC_INST_BNELR, build_bnelr },
        { PPC_INST_BLT, build_blt },
        { PPC_INST_BLTLR, build_bltlr },
        { PPC_INST_BGE, build_bge },
        { PPC_INST_BGELR, build_bgelr },
        { PPC_INST_BGT, build_bgt },
        { PPC_INST_BGTLR, build_bgtlr },
        { PPC_INST_BLE, build_ble },
        { PPC_INST_BLELR, build_blelr },
        { PPC_INST_BSO, build_bso },
        { PPC_INST_BSOLR, build_bsolr },
        { PPC_INST_BNS, build_bns },
        { PPC_INST_BNSLR, build_bnslr },

        //=====================================================================
        // Floating Point
        //=====================================================================
        { PPC_INST_FABS, build_fabs },
        { PPC_INST_FNABS, build_fnabs },
        { PPC_INST_FNEG, build_fneg },
        { PPC_INST_FMR, build_fmr },
        { PPC_INST_FCFID, build_fcfid },
        { PPC_INST_FCTID, build_fctid },
        { PPC_INST_FCTIDZ, build_fctidz },
        { PPC_INST_FCTIWZ, build_fctiwz },
        { PPC_INST_FRSP, build_frsp },
        { PPC_INST_FCMPU, build_fcmpu },
        { PPC_INST_FCMPO, build_fcmpo },
        { PPC_INST_FADD, build_fadd },
        { PPC_INST_FADDS, build_fadds },
        { PPC_INST_FSUB, build_fsub },
        { PPC_INST_FSUBS, build_fsubs },
        { PPC_INST_FMUL, build_fmul },
        { PPC_INST_FMULS, build_fmuls },
        { PPC_INST_FDIV, build_fdiv },
        { PPC_INST_FDIVS, build_fdivs },
        { PPC_INST_FMADD, build_fmadd },
        { PPC_INST_FMADDS, build_fmadds },
        { PPC_INST_FMSUB, build_fmsub },
        { PPC_INST_FMSUBS, build_fmsubs },
        { PPC_INST_FNMADDS, build_fnmadds },
        { PPC_INST_FNMSUB, build_fnmsub },
        { PPC_INST_FNMSUBS, build_fnmsubs },
        { PPC_INST_FRES, build_fres },
        { PPC_INST_FRSQRTE, build_frsqrte },
        { PPC_INST_FSQRT, build_fsqrt },
        { PPC_INST_FSQRTS, build_fsqrts },
        { PPC_INST_FSEL, build_fsel },

        //=====================================================================
        // Memory - Load Immediate
        //=====================================================================
        { PPC_INST_LI, build_li },
        { PPC_INST_LIS, build_lis },

        //=====================================================================
        // Memory - Loads
        //=====================================================================
        { PPC_INST_LBZ, build_lbz },
        { PPC_INST_LBZU, build_lbzu },
        { PPC_INST_LBZX, build_lbzx },
        { PPC_INST_LHA, build_lha },
        { PPC_INST_LHAU, build_lhau },
        { PPC_INST_LHAX, build_lhax },
        { PPC_INST_LHBRX, build_lhbrx },
        { PPC_INST_LHZ, build_lhz },
        { PPC_INST_LHZU, build_lhzu },
        { PPC_INST_LHZUX, build_lhzux },
        { PPC_INST_LHZX, build_lhzx },
        { PPC_INST_LWA, build_lwa },
        { PPC_INST_LWAX, build_lwax },
        { PPC_INST_LWZ, build_lwz },
        { PPC_INST_LWZU, build_lwzu },
        { PPC_INST_LWZUX, build_lwzux },
        { PPC_INST_LWZX, build_lwzx },
        { PPC_INST_LWBRX, build_lwbrx },
        { PPC_INST_LD, build_ld },
        { PPC_INST_LDU, build_ldu },
        { PPC_INST_LDX, build_ldx },
        { PPC_INST_LWARX, build_lwarx },
        { PPC_INST_LDARX, build_ldarx },
        { PPC_INST_LFD, build_lfd },
        { PPC_INST_LFDU, build_lfdu },
        { PPC_INST_LFDUX, build_lfdux },
        { PPC_INST_LFDX, build_lfdx },
        { PPC_INST_LFS, build_lfs },
        { PPC_INST_LFSU, build_lfsu },
        { PPC_INST_LFSUX, build_lfsux },
        { PPC_INST_LFSX, build_lfsx },

        //=====================================================================
        // Memory - Stores
        //=====================================================================
        { PPC_INST_STB, build_stb },
        { PPC_INST_STBU, build_stbu },
        { PPC_INST_STBX, build_stbx },
        { PPC_INST_STH, build_sth },
        { PPC_INST_STHBRX, build_sthbrx },
        { PPC_INST_STHU, build_sthu },
        { PPC_INST_STHUX, build_sthux },
        { PPC_INST_STHX, build_sthx },
        { PPC_INST_STW, build_stw },
        { PPC_INST_STWU, build_stwu },
        { PPC_INST_STWUX, build_stwux },
        { PPC_INST_STWX, build_stwx },
        { PPC_INST_STWBRX, build_stwbrx },
        { PPC_INST_STWCX, build_stwcx },
        { PPC_INST_STDCX, build_stdcx },
        { PPC_INST_STD, build_std },
        { PPC_INST_STDU, build_stdu },
        { PPC_INST_STDX, build_stdx },
        { PPC_INST_STFD, build_stfd },
        { PPC_INST_STFDU, build_stfdu },
        { PPC_INST_STFDX, build_stfdx },
        { PPC_INST_STFIWX, build_stfiwx },
        { PPC_INST_STFS, build_stfs },
        { PPC_INST_STFSU, build_stfsu },
        { PPC_INST_STFSX, build_stfsx },

        //=====================================================================
        // Memory - Vector Loads
        //=====================================================================
        { PPC_INST_LVX, build_lvx },
        { PPC_INST_LVX128, build_lvx },
        { PPC_INST_LVLX, build_lvlx },
        { PPC_INST_LVLX128, build_lvlx },
        { PPC_INST_LVRX, build_lvrx },
        { PPC_INST_LVRX128, build_lvrx },
        { PPC_INST_LVSL, build_lvsl },
        { PPC_INST_LVSR, build_lvsr },
        { PPC_INST_LVEBX, build_lvx },  // Same as LVX for our purposes
        { PPC_INST_LVEHX, build_lvx },  // Same as LVX for our purposes
        { PPC_INST_LVEWX, build_lvx },  // Same as LVX for our purposes
        { PPC_INST_LVEWX128, build_lvx },

        //=====================================================================
        // Memory - Vector Stores
        //=====================================================================
        { PPC_INST_STVEHX, build_stvehx },
        { PPC_INST_STVEWX, build_stvewx },
        { PPC_INST_STVEWX128, build_stvewx },
        { PPC_INST_STVLX, build_stvlx },
        { PPC_INST_STVLX128, build_stvlx },
        { PPC_INST_STVLXL128, build_stvlx },
        { PPC_INST_STVRX, build_stvrx },
        { PPC_INST_STVRX128, build_stvrx },
        { PPC_INST_STVX, build_stvx },
        { PPC_INST_STVX128, build_stvx },

        //=====================================================================
        // System
        //=====================================================================
        { PPC_INST_NOP, build_nop },
        { PPC_INST_ATTN, build_attn },
        { PPC_INST_SYNC, build_sync },
        { PPC_INST_LWSYNC, build_lwsync },
        { PPC_INST_EIEIO, build_eieio },
        { PPC_INST_DB16CYC, build_db16cyc },
        { PPC_INST_CCTPL, build_cctpl },
        { PPC_INST_CCTPM, build_cctpm },
        // Trap word immediate (all variants map to generic TWI)
        { PPC_INST_TWI, build_twi },
        { PPC_INST_TWLGTI, build_twi },
        { PPC_INST_TWLLTI, build_twi },
        { PPC_INST_TWEQI, build_twi },
        { PPC_INST_TWLGEI, build_twi },
        { PPC_INST_TWLNLI, build_twi },
        { PPC_INST_TWLLEI, build_twi },
        { PPC_INST_TWLNGI, build_twi },
        { PPC_INST_TWGTI, build_twi },
        { PPC_INST_TWGEI, build_twi },
        { PPC_INST_TWNLI, build_twi },
        { PPC_INST_TWLTI, build_twi },
        { PPC_INST_TWLEI, build_twi },
        { PPC_INST_TWNGI, build_twi },
        { PPC_INST_TWNEI, build_twi },
        // Trap doubleword immediate (all variants map to generic TDI)
        { PPC_INST_TDI, build_tdi },
        { PPC_INST_TDLGTI, build_tdi },
        { PPC_INST_TDLLTI, build_tdi },
        { PPC_INST_TDEQI, build_tdi },
        { PPC_INST_TDLGEI, build_tdi },
        { PPC_INST_TDLNLI, build_tdi },
        { PPC_INST_TDLLEI, build_tdi },
        { PPC_INST_TDLNGI, build_tdi },
        { PPC_INST_TDGTI, build_tdi },
        { PPC_INST_TDGEI, build_tdi },
        { PPC_INST_TDNLI, build_tdi },
        { PPC_INST_TDLTI, build_tdi },
        { PPC_INST_TDLEI, build_tdi },
        { PPC_INST_TDNGI, build_tdi },
        { PPC_INST_TDNEI, build_tdi },
        // Trap word register (all variants map to generic TW)
        { PPC_INST_TW, build_tw },
        { PPC_INST_TWGE, build_tw },
        { PPC_INST_TWGT, build_tw },
        { PPC_INST_TWLE, build_tw },
        { PPC_INST_TWLT, build_tw },
        { PPC_INST_TWEQ, build_tw },
        { PPC_INST_TWNE, build_tw },
        { PPC_INST_TWLGE, build_tw },
        { PPC_INST_TWLGT, build_tw },
        { PPC_INST_TWLLE, build_tw },
        { PPC_INST_TWLLT, build_tw },
        // Trap doubleword register (all variants map to generic TD)
        { PPC_INST_TD, build_td },
        { PPC_INST_TDGE, build_td },
        { PPC_INST_TDGT, build_td },
        { PPC_INST_TDLE, build_td },
        { PPC_INST_TDLT, build_td },
        { PPC_INST_TDEQ, build_td },
        { PPC_INST_TDNE, build_td },
        { PPC_INST_TDLGE, build_td },
        { PPC_INST_TDLGT, build_td },
        { PPC_INST_TDLLE, build_td },
        { PPC_INST_TDLLT, build_td },
        { PPC_INST_DCBF, build_dcbf },
        { PPC_INST_DCBT, build_dcbt },
        { PPC_INST_DCBTST, build_dcbtst },
        { PPC_INST_DCBZ, build_dcbz },
        { PPC_INST_DCBZL, build_dcbzl },
        { PPC_INST_DCBST, build_dcbst },
        { PPC_INST_MR, build_mr },
        { PPC_INST_MFCR, build_mfcr },
        { PPC_INST_MFOCRF, build_mfocrf },
        { PPC_INST_MFLR, build_mflr },
        { PPC_INST_MFMSR, build_mfmsr },
        { PPC_INST_MFFS, build_mffs },
        { PPC_INST_MFTB, build_mftb },
        { PPC_INST_MTCR, build_mtcr },
        { PPC_INST_MTCTR, build_mtctr },
        { PPC_INST_MTLR, build_mtlr },
        { PPC_INST_MTMSRD, build_mtmsrd },
        { PPC_INST_MTFSF, build_mtfsf },
        { PPC_INST_MTXER, build_mtxer },
        { PPC_INST_CLRLDI, build_clrldi },

        //=====================================================================
        // Vector - Floating Point Arithmetic
        //=====================================================================
        { PPC_INST_VADDFP, build_vaddfp },
        { PPC_INST_VADDFP128, build_vaddfp },
        { PPC_INST_VSUBFP, build_vsubfp },
        { PPC_INST_VSUBFP128, build_vsubfp },
        { PPC_INST_VMULFP128, build_vmulfp128 },
        { PPC_INST_VMADDFP, build_vmaddfp },
        { PPC_INST_VMADDFP128, build_vmaddfp },
        { PPC_INST_VMADDCFP128, build_vmaddfp },  // Same as VMADDFP
        { PPC_INST_VNMSUBFP, build_vnmsubfp },
        { PPC_INST_VNMSUBFP128, build_vnmsubfp },
        { PPC_INST_VMAXFP, build_vmaxfp },
        { PPC_INST_VMAXFP128, build_vmaxfp },
        { PPC_INST_VMINFP, build_vminfp },
        { PPC_INST_VMINFP128, build_vminfp },
        { PPC_INST_VREFP, build_vrefp },
        { PPC_INST_VREFP128, build_vrefp },
        { PPC_INST_VRSQRTEFP, build_vrsqrtefp },
        { PPC_INST_VRSQRTEFP128, build_vrsqrtefp },
        { PPC_INST_VEXPTEFP, build_vexptefp },
        { PPC_INST_VEXPTEFP128, build_vexptefp },
        { PPC_INST_VLOGEFP, build_vlogefp },
        { PPC_INST_VLOGEFP128, build_vlogefp },

        //=====================================================================
        // Vector - Dot Products
        //=====================================================================
        { PPC_INST_VMSUM3FP128, build_vmsum3fp128 },
        { PPC_INST_VMSUM4FP128, build_vmsum4fp128 },

        //=====================================================================
        // Vector - Rounding
        //=====================================================================
        { PPC_INST_VRFIM, build_vrfim },
        { PPC_INST_VRFIM128, build_vrfim },
        { PPC_INST_VRFIN, build_vrfin },
        { PPC_INST_VRFIN128, build_vrfin },
        { PPC_INST_VRFIP, build_vrfip },
        { PPC_INST_VRFIP128, build_vrfip },
        { PPC_INST_VRFIZ, build_vrfiz },
        { PPC_INST_VRFIZ128, build_vrfiz },

        //=====================================================================
        // Vector - Integer Arithmetic
        //=====================================================================
        { PPC_INST_VADDSBS, build_vaddsbs },
        { PPC_INST_VADDSHS, build_vaddshs },
        { PPC_INST_VADDSWS, build_vaddsws },
        { PPC_INST_VADDUBM, build_vaddubm },
        { PPC_INST_VADDUBS, build_vaddubs },
        { PPC_INST_VADDUHM, build_vadduhm },
        { PPC_INST_VADDUWM, build_vadduwm },
        { PPC_INST_VADDUWS, build_vadduws },
        { PPC_INST_VSUBSWS, build_vsubsws },
        { PPC_INST_VSUBUBS, build_vsububs },
        { PPC_INST_VSUBUWS, build_vsubuws },
        { PPC_INST_VSUBUHM, build_vsubuhm },
        { PPC_INST_VSUBSHS, build_vsubshs },
        { PPC_INST_VMAXSW, build_vmaxsw },
        { PPC_INST_VMAXSH, build_vmaxsh },
        { PPC_INST_VMINSH, build_vminsh },
        { PPC_INST_VMAXUH, build_vmaxuh },
        { PPC_INST_VMINUH, build_vminuh },

        //=====================================================================
        // Vector - Average
        //=====================================================================
        { PPC_INST_VAVGSB, build_vavgsb },
        { PPC_INST_VAVGSH, build_vavgsh },
        { PPC_INST_VAVGUB, build_vavgub },
        { PPC_INST_VAVGUH, build_vavguh },

        //=====================================================================
        // Vector - Logical
        //=====================================================================
        { PPC_INST_VAND, build_vand },
        { PPC_INST_VAND128, build_vand },
        { PPC_INST_VANDC, build_vandc },
        { PPC_INST_VANDC128, build_vandc128 },
        { PPC_INST_VOR, build_vor },
        { PPC_INST_VOR128, build_vor },
        { PPC_INST_VXOR, build_vxor },
        { PPC_INST_VXOR128, build_vxor },
        { PPC_INST_VNOR, build_vnor },
        { PPC_INST_VNOR128, build_vnor },
        { PPC_INST_VSEL, build_vsel },
        { PPC_INST_VSEL128, build_vsel },

        //=====================================================================
        // Vector - Compare
        //=====================================================================
        { PPC_INST_VCMPBFP, build_vcmpbfp },
        { PPC_INST_VCMPBFP128, build_vcmpbfp },
        { PPC_INST_VCMPEQFP, build_vcmpeqfp },
        { PPC_INST_VCMPEQFP128, build_vcmpeqfp },
        { PPC_INST_VCMPEQUB, build_vcmpequb },
        { PPC_INST_VCMPEQUW, build_vcmpequw },
        { PPC_INST_VCMPEQUW128, build_vcmpequw },
        { PPC_INST_VCMPGEFP, build_vcmpgefp },
        { PPC_INST_VCMPGEFP128, build_vcmpgefp },
        { PPC_INST_VCMPGTFP, build_vcmpgtfp },
        { PPC_INST_VCMPGTFP128, build_vcmpgtfp },
        { PPC_INST_VCMPGTUB, build_vcmpgtub },
        { PPC_INST_VCMPGTUH, build_vcmpgtuh },
        { PPC_INST_VCMPGTSH, build_vcmpgtsh },
        { PPC_INST_VCMPGTSW, build_vcmpgtsw },

        //=====================================================================
        // Vector - Conversion
        //=====================================================================
        { PPC_INST_VCTSXS, build_vctsxs },
        { PPC_INST_VCFPSXWS128, build_vctsxs },  // Alias
        { PPC_INST_VCTUXS, build_vctuxs },
        { PPC_INST_VCFPUXWS128, build_vctuxs },  // Alias
        { PPC_INST_VCFSX, build_vcfsx },
        { PPC_INST_VCSXWFP128, build_vcfsx },    // Alias
        { PPC_INST_VCFUX, build_vcfux },
        { PPC_INST_VCUXWFP128, build_vcfux },    // Alias

        //=====================================================================
        // Vector - Merge
        //=====================================================================
        { PPC_INST_VMRGHB, build_vmrghb },
        { PPC_INST_VMRGHH, build_vmrghh },
        { PPC_INST_VMRGHW, build_vmrghw },
        { PPC_INST_VMRGHW128, build_vmrghw },
        { PPC_INST_VMRGLB, build_vmrglb },
        { PPC_INST_VMRGLH, build_vmrglh },
        { PPC_INST_VMRGLW, build_vmrglw },
        { PPC_INST_VMRGLW128, build_vmrglw },

        //=====================================================================
        // Vector - Permute
        //=====================================================================
        { PPC_INST_VPERM, build_vperm },
        { PPC_INST_VPERM128, build_vperm },
        { PPC_INST_VPERMWI128, build_vpermwi128 },
        { PPC_INST_VRLIMI128, build_vrlimi128 },

        //=====================================================================
        // Vector - Shift
        //=====================================================================
        { PPC_INST_VSL, build_vsl },
        { PPC_INST_VSLB, build_vslb },
        { PPC_INST_VSLH, build_vslh },
        { PPC_INST_VSLDOI, build_vsldoi },
        { PPC_INST_VSLDOI128, build_vsldoi },
        { PPC_INST_VSLW, build_vslw },
        { PPC_INST_VSLW128, build_vslw },
        { PPC_INST_VSLO, build_vslo },
        { PPC_INST_VSLO128, build_vslo },
        { PPC_INST_VSR, build_vsr },
        { PPC_INST_VSRH, build_vsrh },
        { PPC_INST_VSRAH, build_vsrah },
        { PPC_INST_VSRAW, build_vsraw },
        { PPC_INST_VSRAW128, build_vsraw },
        { PPC_INST_VSRW, build_vsrw },
        { PPC_INST_VSRW128, build_vsrw },
        { PPC_INST_VSRO, build_vsro },
        { PPC_INST_VSRO128, build_vsro },
        { PPC_INST_VRLH, build_vrlh },

        //=====================================================================
        // Vector - Splat
        //=====================================================================
        { PPC_INST_VSPLTB, build_vspltb },
        { PPC_INST_VSPLTH, build_vsplth },
        { PPC_INST_VSPLTISB, build_vspltisb },
        { PPC_INST_VSPLTISH, build_vspltish },
        { PPC_INST_VSPLTISW, build_vspltisw },
        { PPC_INST_VSPLTISW128, build_vspltisw },
        { PPC_INST_VSPLTW, build_vspltw },
        { PPC_INST_VSPLTW128, build_vspltw },

        //=====================================================================
        // Vector - Pack
        //=====================================================================
        { PPC_INST_VPKUHUM, build_vpkuhum },
        { PPC_INST_VPKUHUM128, build_vpkuhum },
        { PPC_INST_VPKUHUS, build_vpkuhus },
        { PPC_INST_VPKUHUS128, build_vpkuhus },
        { PPC_INST_VPKUWUM, build_vpkuwum },
        { PPC_INST_VPKUWUM128, build_vpkuwum },
        { PPC_INST_VPKUWUS, build_vpkuwus },
        { PPC_INST_VPKUWUS128, build_vpkuwus },
        { PPC_INST_VPKSHSS, build_vpkshss },
        { PPC_INST_VPKSHSS128, build_vpkshss },
        { PPC_INST_VPKSHUS, build_vpkshus },
        { PPC_INST_VPKSHUS128, build_vpkshus },
        { PPC_INST_VPKSWSS, build_vpkswss },
        { PPC_INST_VPKSWSS128, build_vpkswss },
        { PPC_INST_VPKSWUS, build_vpkswus },
        { PPC_INST_VPKSWUS128, build_vpkswus },
        { PPC_INST_VPKD3D128, build_vpkd3d128 },

        //=====================================================================
        // Vector - Unpack
        //=====================================================================
        { PPC_INST_VUPKD3D128, build_vupkd3d128 },
        { PPC_INST_VUPKHSB, build_vupkhsb },
        { PPC_INST_VUPKHSB128, build_vupkhsb },
        { PPC_INST_VUPKHSH, build_vupkhsh },
        { PPC_INST_VUPKHSH128, build_vupkhsh },
        { PPC_INST_VUPKLSB, build_vupklsb },
        { PPC_INST_VUPKLSB128, build_vupklsb },
        { PPC_INST_VUPKLSH, build_vupklsh },
        { PPC_INST_VUPKLSH128, build_vupklsh },
    };
    return table;
}

bool DispatchInstruction(int id, BuilderContext& ctx)
{
    const auto& table = GetDispatchTable();
    auto it = table.find(id);
    if (it != table.end())
    {
        return it->second(ctx);
    }

    // Emit trap code for unimplemented instruction - allows tests to be generated
    // and fail at runtime rather than skipping the entire function
    REXCODEGEN_WARN("Unimplemented: {} at 0x{:08X}", ctx.insn.opcode->name, ctx.base);
    ctx.println("\t// UNIMPLEMENTED: {}", ctx.insn.opcode->name);
    ctx.println("\tPPC_UNIMPLEMENTED(0x{:X}, \"{}\");", ctx.base, ctx.insn.opcode->name);
    return true;
}

} // namespace rex::codegen
