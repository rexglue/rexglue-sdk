/**
 * @file        rexcodegen/internal/builders.h
 * @brief       Code builder interface definitions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

namespace rex::codegen {

struct BuilderContext;

/**
 * Build C++ code for a PPC instruction using the dispatch table.
 *
 * @param id The PPC instruction ID (PPC_INST_*)
 * @param ctx The builder context
 * @return true if instruction was handled, false if unknown
 */
bool DispatchInstruction(int id, BuilderContext& ctx);

//=============================================================================
// Comparison Builders (CMP*, CMPL*)
//=============================================================================

bool build_cmpd(BuilderContext& ctx);
bool build_cmpdi(BuilderContext& ctx);
bool build_cmpld(BuilderContext& ctx);
bool build_cmpldi(BuilderContext& ctx);
bool build_cmplw(BuilderContext& ctx);
bool build_cmplwi(BuilderContext& ctx);
bool build_cmpw(BuilderContext& ctx);
bool build_cmpwi(BuilderContext& ctx);

//=============================================================================
// Arithmetic Builders (ADD, SUB, MUL, DIV, NEG)
//=============================================================================

// Addition
bool build_add(BuilderContext& ctx);
bool build_addc(BuilderContext& ctx);
bool build_adde(BuilderContext& ctx);
bool build_addi(BuilderContext& ctx);
bool build_addic(BuilderContext& ctx);
bool build_addis(BuilderContext& ctx);
bool build_addme(BuilderContext& ctx);
bool build_addze(BuilderContext& ctx);

// Division
bool build_divd(BuilderContext& ctx);
bool build_divdu(BuilderContext& ctx);
bool build_divw(BuilderContext& ctx);
bool build_divwu(BuilderContext& ctx);

// Multiplication
bool build_mulhd(BuilderContext& ctx);
bool build_mulhdu(BuilderContext& ctx);
bool build_mulhw(BuilderContext& ctx);
bool build_mulhwu(BuilderContext& ctx);
bool build_mulld(BuilderContext& ctx);
bool build_mulli(BuilderContext& ctx);
bool build_mullw(BuilderContext& ctx);

// Negation
bool build_neg(BuilderContext& ctx);

// Subtraction
bool build_subf(BuilderContext& ctx);
bool build_subfc(BuilderContext& ctx);
bool build_subfe(BuilderContext& ctx);
bool build_subfic(BuilderContext& ctx);
bool build_subfme(BuilderContext& ctx);
bool build_subfze(BuilderContext& ctx);

//=============================================================================
// Logical Builders (AND, OR, XOR, shifts, rotates, bit manipulation)
//=============================================================================

// AND operations
bool build_and(BuilderContext& ctx);
bool build_andc(BuilderContext& ctx);
bool build_andi(BuilderContext& ctx);
bool build_andis(BuilderContext& ctx);

// OR operations
bool build_nand(BuilderContext& ctx);
bool build_nor(BuilderContext& ctx);
bool build_not(BuilderContext& ctx);
bool build_or(BuilderContext& ctx);
bool build_orc(BuilderContext& ctx);
bool build_ori(BuilderContext& ctx);
bool build_oris(BuilderContext& ctx);

// XOR operations
bool build_xor(BuilderContext& ctx);
bool build_xori(BuilderContext& ctx);
bool build_xoris(BuilderContext& ctx);

// Equivalence (XNOR)
bool build_eqv(BuilderContext& ctx);

// Count leading zeros
bool build_cntlzd(BuilderContext& ctx);
bool build_cntlzw(BuilderContext& ctx);

// Sign extension
bool build_extsb(BuilderContext& ctx);
bool build_extsh(BuilderContext& ctx);
bool build_extsw(BuilderContext& ctx);

// Clear operations
bool build_clrlwi(BuilderContext& ctx);

// Rotate left double word
bool build_rldicl(BuilderContext& ctx);
bool build_rldicr(BuilderContext& ctx);
bool build_rldimi(BuilderContext& ctx);
bool build_rotldi(BuilderContext& ctx);

// Rotate left word
bool build_rlwimi(BuilderContext& ctx);
bool build_rlwinm(BuilderContext& ctx);
bool build_rlwnm(BuilderContext& ctx);
bool build_rotlw(BuilderContext& ctx);
bool build_rotlwi(BuilderContext& ctx);

// Shift left
bool build_sld(BuilderContext& ctx);
bool build_slw(BuilderContext& ctx);

// Shift right algebraic
bool build_srad(BuilderContext& ctx);
bool build_sradi(BuilderContext& ctx);
bool build_sraw(BuilderContext& ctx);
bool build_srawi(BuilderContext& ctx);

// Shift right logical
bool build_srd(BuilderContext& ctx);
bool build_srw(BuilderContext& ctx);

//=============================================================================
// Control Flow Builders (branches, calls, returns)
//=============================================================================

// Unconditional branch
bool build_b(BuilderContext& ctx);
bool build_bl(BuilderContext& ctx);
bool build_blr(BuilderContext& ctx);
bool build_blrl(BuilderContext& ctx);

// Count register branch
bool build_bctr(BuilderContext& ctx);
bool build_bctrl(BuilderContext& ctx);
bool build_bnectr(BuilderContext& ctx);

// Decrement counter and branch
bool build_bdz(BuilderContext& ctx);
bool build_bdzf(BuilderContext& ctx);
bool build_bdzlr(BuilderContext& ctx);
bool build_bdnz(BuilderContext& ctx);
bool build_bdnzf(BuilderContext& ctx);
bool build_bdnzt(BuilderContext& ctx);

// Conditional branch (eq)
bool build_beq(BuilderContext& ctx);
bool build_beqlr(BuilderContext& ctx);
bool build_bne(BuilderContext& ctx);
bool build_bnelr(BuilderContext& ctx);

// Conditional branch (lt)
bool build_blt(BuilderContext& ctx);
bool build_bltlr(BuilderContext& ctx);
bool build_bge(BuilderContext& ctx);
bool build_bgelr(BuilderContext& ctx);

// Conditional branch (gt)
bool build_bgt(BuilderContext& ctx);
bool build_bgtlr(BuilderContext& ctx);
bool build_ble(BuilderContext& ctx);
bool build_blelr(BuilderContext& ctx);

// Conditional branch (so - summary overflow / unordered)
bool build_bso(BuilderContext& ctx);
bool build_bsolr(BuilderContext& ctx);
bool build_bns(BuilderContext& ctx);
bool build_bnslr(BuilderContext& ctx);

//=============================================================================
// Floating Point Builders
//=============================================================================

// Sign manipulation
bool build_fabs(BuilderContext& ctx);
bool build_fnabs(BuilderContext& ctx);
bool build_fneg(BuilderContext& ctx);

// Move and conversion
bool build_fmr(BuilderContext& ctx);
bool build_fcfid(BuilderContext& ctx);
bool build_fctid(BuilderContext& ctx);
bool build_fctidz(BuilderContext& ctx);
bool build_fctiwz(BuilderContext& ctx);
bool build_frsp(BuilderContext& ctx);

// Comparison
bool build_fcmpu(BuilderContext& ctx);
bool build_fcmpo(BuilderContext& ctx);

// Addition
bool build_fadd(BuilderContext& ctx);
bool build_fadds(BuilderContext& ctx);

// Subtraction
bool build_fsub(BuilderContext& ctx);
bool build_fsubs(BuilderContext& ctx);

// Multiplication
bool build_fmul(BuilderContext& ctx);
bool build_fmuls(BuilderContext& ctx);

// Division
bool build_fdiv(BuilderContext& ctx);
bool build_fdivs(BuilderContext& ctx);

// Fused multiply-add
bool build_fmadd(BuilderContext& ctx);
bool build_fmadds(BuilderContext& ctx);
bool build_fmsub(BuilderContext& ctx);
bool build_fmsubs(BuilderContext& ctx);
bool build_fnmadds(BuilderContext& ctx);
bool build_fnmsub(BuilderContext& ctx);
bool build_fnmsubs(BuilderContext& ctx);

// Reciprocal and square root
bool build_fres(BuilderContext& ctx);
bool build_frsqrte(BuilderContext& ctx);
bool build_fsqrt(BuilderContext& ctx);
bool build_fsqrts(BuilderContext& ctx);

// Selection
bool build_fsel(BuilderContext& ctx);

//=============================================================================
// Memory Builders (loads and stores)
//=============================================================================

// Load immediate
bool build_li(BuilderContext& ctx);
bool build_lis(BuilderContext& ctx);

// Byte loads
bool build_lbz(BuilderContext& ctx);
bool build_lbzu(BuilderContext& ctx);
bool build_lbzx(BuilderContext& ctx);

// Halfword loads
bool build_lha(BuilderContext& ctx);
bool build_lhau(BuilderContext& ctx);
bool build_lhax(BuilderContext& ctx);
bool build_lhbrx(BuilderContext& ctx);
bool build_lhz(BuilderContext& ctx);
bool build_lhzu(BuilderContext& ctx);
bool build_lhzux(BuilderContext& ctx);
bool build_lhzx(BuilderContext& ctx);

// Word loads
bool build_lwa(BuilderContext& ctx);
bool build_lwax(BuilderContext& ctx);
bool build_lwbrx(BuilderContext& ctx);
bool build_lwz(BuilderContext& ctx);
bool build_lwzu(BuilderContext& ctx);
bool build_lwzux(BuilderContext& ctx);
bool build_lwzx(BuilderContext& ctx);

// Doubleword loads
bool build_ld(BuilderContext& ctx);
bool build_ldu(BuilderContext& ctx);
bool build_ldx(BuilderContext& ctx);

// Atomic load and reserve
bool build_lwarx(BuilderContext& ctx);
bool build_ldarx(BuilderContext& ctx);

// Floating point loads
bool build_lfd(BuilderContext& ctx);
bool build_lfdu(BuilderContext& ctx);
bool build_lfdux(BuilderContext& ctx);
bool build_lfdx(BuilderContext& ctx);
bool build_lfs(BuilderContext& ctx);
bool build_lfsu(BuilderContext& ctx);
bool build_lfsux(BuilderContext& ctx);
bool build_lfsx(BuilderContext& ctx);

// Byte stores
bool build_stb(BuilderContext& ctx);
bool build_stbu(BuilderContext& ctx);
bool build_stbx(BuilderContext& ctx);

// Halfword stores
bool build_sth(BuilderContext& ctx);
bool build_sthbrx(BuilderContext& ctx);
bool build_sthu(BuilderContext& ctx);
bool build_sthux(BuilderContext& ctx);
bool build_sthx(BuilderContext& ctx);

// Word stores
bool build_stw(BuilderContext& ctx);
bool build_stwu(BuilderContext& ctx);
bool build_stwux(BuilderContext& ctx);
bool build_stwx(BuilderContext& ctx);
bool build_stwbrx(BuilderContext& ctx);

// Atomic store conditional
bool build_stwcx(BuilderContext& ctx);
bool build_stdcx(BuilderContext& ctx);

// Doubleword stores
bool build_std(BuilderContext& ctx);
bool build_stdu(BuilderContext& ctx);
bool build_stdx(BuilderContext& ctx);

// Floating point stores
bool build_stfd(BuilderContext& ctx);
bool build_stfdu(BuilderContext& ctx);
bool build_stfdx(BuilderContext& ctx);
bool build_stfiwx(BuilderContext& ctx);
bool build_stfs(BuilderContext& ctx);
bool build_stfsu(BuilderContext& ctx);
bool build_stfsx(BuilderContext& ctx);

// Vector loads
bool build_lvx(BuilderContext& ctx);
bool build_lvlx(BuilderContext& ctx);
bool build_lvrx(BuilderContext& ctx);
bool build_lvsl(BuilderContext& ctx);
bool build_lvsr(BuilderContext& ctx);

// Vector stores
bool build_stvehx(BuilderContext& ctx);
bool build_stvewx(BuilderContext& ctx);
bool build_stvlx(BuilderContext& ctx);
bool build_stvrx(BuilderContext& ctx);
bool build_stvx(BuilderContext& ctx);

//=============================================================================
// System Builders (NOP, SYNC, MF*, MT*, DC*, trap)
//=============================================================================

// No-ops and sync
bool build_nop(BuilderContext& ctx);
bool build_attn(BuilderContext& ctx);
bool build_sync(BuilderContext& ctx);
bool build_lwsync(BuilderContext& ctx);
bool build_eieio(BuilderContext& ctx);
bool build_db16cyc(BuilderContext& ctx);
bool build_cctpl(BuilderContext& ctx);
bool build_cctpm(BuilderContext& ctx);

// Trap instructions (generic builders - all specific variants map to these)
bool build_twi(BuilderContext& ctx);  // Trap word immediate
bool build_tdi(BuilderContext& ctx);  // Trap doubleword immediate
bool build_tw(BuilderContext& ctx);   // Trap word register
bool build_td(BuilderContext& ctx);   // Trap doubleword register

// Cache operations
bool build_dcbf(BuilderContext& ctx);
bool build_dcbt(BuilderContext& ctx);
bool build_dcbtst(BuilderContext& ctx);
bool build_dcbz(BuilderContext& ctx);
bool build_dcbzl(BuilderContext& ctx);
bool build_dcbst(BuilderContext& ctx);

// Move register
bool build_mr(BuilderContext& ctx);

// Move from special registers
bool build_mfcr(BuilderContext& ctx);
bool build_mfocrf(BuilderContext& ctx);
bool build_mflr(BuilderContext& ctx);
bool build_mfmsr(BuilderContext& ctx);
bool build_mffs(BuilderContext& ctx);
bool build_mftb(BuilderContext& ctx);

// Move to special registers
bool build_mtcr(BuilderContext& ctx);
bool build_mtctr(BuilderContext& ctx);
bool build_mtlr(BuilderContext& ctx);
bool build_mtmsrd(BuilderContext& ctx);
bool build_mtfsf(BuilderContext& ctx);
bool build_mtxer(BuilderContext& ctx);

// Clear left double word immediate
bool build_clrldi(BuilderContext& ctx);

//=============================================================================
// Vector Builders (AltiVec/VMX instructions)
//=============================================================================

// Vector floating point arithmetic
bool build_vaddfp(BuilderContext& ctx);
bool build_vsubfp(BuilderContext& ctx);
bool build_vmulfp128(BuilderContext& ctx);
bool build_vmaddfp(BuilderContext& ctx);
bool build_vnmsubfp(BuilderContext& ctx);
bool build_vmaxfp(BuilderContext& ctx);
bool build_vminfp(BuilderContext& ctx);
bool build_vrefp(BuilderContext& ctx);
bool build_vrsqrtefp(BuilderContext& ctx);
bool build_vexptefp(BuilderContext& ctx);
bool build_vlogefp(BuilderContext& ctx);

// Vector dot products
bool build_vmsum3fp128(BuilderContext& ctx);
bool build_vmsum4fp128(BuilderContext& ctx);

// Vector rounding
bool build_vrfim(BuilderContext& ctx);
bool build_vrfin(BuilderContext& ctx);
bool build_vrfiz(BuilderContext& ctx);

// Vector integer arithmetic
bool build_vaddshs(BuilderContext& ctx);
bool build_vaddsws(BuilderContext& ctx);
bool build_vaddubm(BuilderContext& ctx);
bool build_vaddubs(BuilderContext& ctx);
bool build_vadduhm(BuilderContext& ctx);
bool build_vadduwm(BuilderContext& ctx);
bool build_vadduws(BuilderContext& ctx);
bool build_vsubshs(BuilderContext& ctx);
bool build_vsubsws(BuilderContext& ctx);
bool build_vsububs(BuilderContext& ctx);
bool build_vsubuhm(BuilderContext& ctx);
bool build_vmaxsh(BuilderContext& ctx);
bool build_vmaxsw(BuilderContext& ctx);
bool build_vmaxuh(BuilderContext& ctx);
bool build_vminsh(BuilderContext& ctx);
bool build_vminuh(BuilderContext& ctx);

// Vector average
bool build_vavgsb(BuilderContext& ctx);
bool build_vavgsh(BuilderContext& ctx);
bool build_vavgub(BuilderContext& ctx);
bool build_vavguh(BuilderContext& ctx);

// Vector logical
bool build_vand(BuilderContext& ctx);
bool build_vandc(BuilderContext& ctx);
bool build_vandc128(BuilderContext& ctx);
bool build_vor(BuilderContext& ctx);
bool build_vxor(BuilderContext& ctx);
bool build_vnor(BuilderContext& ctx);
bool build_vsel(BuilderContext& ctx);

// Vector compare
bool build_vcmpbfp(BuilderContext& ctx);
bool build_vcmpeqfp(BuilderContext& ctx);
bool build_vcmpequb(BuilderContext& ctx);
bool build_vcmpequw(BuilderContext& ctx);
bool build_vcmpgefp(BuilderContext& ctx);
bool build_vcmpgtfp(BuilderContext& ctx);
bool build_vcmpgtub(BuilderContext& ctx);
bool build_vcmpgtuh(BuilderContext& ctx);

// Vector conversion
bool build_vctsxs(BuilderContext& ctx);
bool build_vctuxs(BuilderContext& ctx);
bool build_vcfsx(BuilderContext& ctx);
bool build_vcfux(BuilderContext& ctx);

// Vector merge
bool build_vmrghb(BuilderContext& ctx);
bool build_vmrghh(BuilderContext& ctx);
bool build_vmrghw(BuilderContext& ctx);
bool build_vmrglb(BuilderContext& ctx);
bool build_vmrglh(BuilderContext& ctx);
bool build_vmrglw(BuilderContext& ctx);

// Vector permute
bool build_vperm(BuilderContext& ctx);
bool build_vpermwi128(BuilderContext& ctx);
bool build_vrlimi128(BuilderContext& ctx);

// Vector shift
bool build_vsl(BuilderContext& ctx);
bool build_vslb(BuilderContext& ctx);
bool build_vslh(BuilderContext& ctx);
bool build_vsldoi(BuilderContext& ctx);
bool build_vslw(BuilderContext& ctx);
bool build_vslo(BuilderContext& ctx);
bool build_vsr(BuilderContext& ctx);
bool build_vsrh(BuilderContext& ctx);
bool build_vsrah(BuilderContext& ctx);
bool build_vsraw(BuilderContext& ctx);
bool build_vsrw(BuilderContext& ctx);
bool build_vsro(BuilderContext& ctx);
bool build_vrlh(BuilderContext& ctx);

// Vector splat
bool build_vspltb(BuilderContext& ctx);
bool build_vsplth(BuilderContext& ctx);
bool build_vspltisb(BuilderContext& ctx);
bool build_vspltish(BuilderContext& ctx);
bool build_vspltisw(BuilderContext& ctx);
bool build_vspltw(BuilderContext& ctx);

// Vector pack
bool build_vpkuhum(BuilderContext& ctx);
bool build_vpkuhus(BuilderContext& ctx);
bool build_vpkuwum(BuilderContext& ctx);
bool build_vpkuwus(BuilderContext& ctx);
bool build_vpkshss(BuilderContext& ctx);
bool build_vpkshus(BuilderContext& ctx);
bool build_vpkswss(BuilderContext& ctx);
bool build_vpkswus(BuilderContext& ctx);
bool build_vpkd3d128(BuilderContext& ctx);

// Vector unpack
bool build_vupkd3d128(BuilderContext& ctx);
bool build_vupkhsb(BuilderContext& ctx);
bool build_vupkhsh(BuilderContext& ctx);
bool build_vupklsb(BuilderContext& ctx);
bool build_vupklsh(BuilderContext& ctx);

} // namespace rex::codegen
