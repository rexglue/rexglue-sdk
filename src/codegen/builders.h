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

bool BuildCmpd(BuilderContext& ctx);
bool BuildCmpdi(BuilderContext& ctx);
bool BuildCmpld(BuilderContext& ctx);
bool BuildCmpldi(BuilderContext& ctx);
bool BuildCmplw(BuilderContext& ctx);
bool BuildCmplwi(BuilderContext& ctx);
bool BuildCmpw(BuilderContext& ctx);
bool BuildCmpwi(BuilderContext& ctx);

bool BuildAdd(BuilderContext& ctx);
bool BuildAddc(BuilderContext& ctx);
bool BuildAdde(BuilderContext& ctx);
bool BuildAddi(BuilderContext& ctx);
bool BuildAddic(BuilderContext& ctx);
bool BuildAddis(BuilderContext& ctx);
bool BuildAddme(BuilderContext& ctx);
bool BuildAddze(BuilderContext& ctx);

bool BuildDivd(BuilderContext& ctx);
bool BuildDivdu(BuilderContext& ctx);
bool BuildDivw(BuilderContext& ctx);
bool BuildDivwu(BuilderContext& ctx);

bool BuildMulhd(BuilderContext& ctx);
bool BuildMulhdu(BuilderContext& ctx);
bool BuildMulhw(BuilderContext& ctx);
bool BuildMulhwu(BuilderContext& ctx);
bool BuildMulld(BuilderContext& ctx);
bool BuildMulli(BuilderContext& ctx);
bool BuildMullw(BuilderContext& ctx);

bool BuildNeg(BuilderContext& ctx);

bool BuildSubf(BuilderContext& ctx);
bool BuildSubfc(BuilderContext& ctx);
bool BuildSubfe(BuilderContext& ctx);
bool BuildSubfic(BuilderContext& ctx);
bool BuildSubfme(BuilderContext& ctx);
bool BuildSubfze(BuilderContext& ctx);

bool BuildAnd(BuilderContext& ctx);
bool BuildAndc(BuilderContext& ctx);
bool BuildAndi(BuilderContext& ctx);
bool BuildAndis(BuilderContext& ctx);

bool BuildNand(BuilderContext& ctx);
bool BuildNor(BuilderContext& ctx);
bool BuildNot(BuilderContext& ctx);
bool BuildOr(BuilderContext& ctx);
bool BuildOrc(BuilderContext& ctx);
bool BuildOri(BuilderContext& ctx);
bool BuildOris(BuilderContext& ctx);

bool BuildXor(BuilderContext& ctx);
bool BuildXori(BuilderContext& ctx);
bool BuildXoris(BuilderContext& ctx);

bool BuildCrand(BuilderContext& ctx);
bool BuildCrandc(BuilderContext& ctx);
bool BuildCreqv(BuilderContext& ctx);
bool BuildCrnand(BuilderContext& ctx);
bool BuildCrnor(BuilderContext& ctx);
bool BuildCror(BuilderContext& ctx);
bool BuildCrorc(BuilderContext& ctx);
bool BuildCrxor(BuilderContext& ctx);

bool BuildEqv(BuilderContext& ctx);

bool BuildCntlzd(BuilderContext& ctx);
bool BuildCntlzw(BuilderContext& ctx);

bool BuildExtsb(BuilderContext& ctx);
bool BuildExtsh(BuilderContext& ctx);
bool BuildExtsw(BuilderContext& ctx);

bool BuildClrlwi(BuilderContext& ctx);

bool BuildRldcl(BuilderContext& ctx);
bool BuildRldcr(BuilderContext& ctx);
bool BuildRldic(BuilderContext& ctx);
bool BuildRldicl(BuilderContext& ctx);
bool BuildRldicr(BuilderContext& ctx);
bool BuildRldimi(BuilderContext& ctx);
bool BuildRotldi(BuilderContext& ctx);

bool BuildRlwimi(BuilderContext& ctx);
bool BuildRlwinm(BuilderContext& ctx);
bool BuildRlwnm(BuilderContext& ctx);
bool BuildRotlw(BuilderContext& ctx);
bool BuildRotlwi(BuilderContext& ctx);

bool BuildSld(BuilderContext& ctx);
bool BuildSlw(BuilderContext& ctx);

bool BuildSrad(BuilderContext& ctx);
bool BuildSradi(BuilderContext& ctx);
bool BuildSraw(BuilderContext& ctx);
bool BuildSrawi(BuilderContext& ctx);

bool BuildSrd(BuilderContext& ctx);
bool BuildSrw(BuilderContext& ctx);

bool BuildB(BuilderContext& ctx);
bool BuildBl(BuilderContext& ctx);
bool BuildBlr(BuilderContext& ctx);
bool BuildBlrl(BuilderContext& ctx);

bool BuildBctr(BuilderContext& ctx);
bool BuildBctrl(BuilderContext& ctx);
bool BuildBnectr(BuilderContext& ctx);

bool BuildBdz(BuilderContext& ctx);
bool BuildBdzf(BuilderContext& ctx);
bool BuildBdzlr(BuilderContext& ctx);
bool BuildBdnz(BuilderContext& ctx);
bool BuildBdnzf(BuilderContext& ctx);
bool BuildBdnzlr(BuilderContext& ctx);
bool BuildBdnzt(BuilderContext& ctx);

bool BuildBeq(BuilderContext& ctx);
bool BuildBeqlr(BuilderContext& ctx);
bool BuildBne(BuilderContext& ctx);
bool BuildBnelr(BuilderContext& ctx);

bool BuildBlt(BuilderContext& ctx);
bool BuildBltlr(BuilderContext& ctx);
bool BuildBge(BuilderContext& ctx);
bool BuildBgelr(BuilderContext& ctx);

bool BuildBgt(BuilderContext& ctx);
bool BuildBgtlr(BuilderContext& ctx);
bool BuildBle(BuilderContext& ctx);
bool BuildBlelr(BuilderContext& ctx);

bool BuildBso(BuilderContext& ctx);
bool BuildBsolr(BuilderContext& ctx);
bool BuildBns(BuilderContext& ctx);
bool BuildBnslr(BuilderContext& ctx);

bool BuildFabs(BuilderContext& ctx);
bool BuildFnabs(BuilderContext& ctx);
bool BuildFneg(BuilderContext& ctx);

bool BuildFmr(BuilderContext& ctx);
bool BuildFcfid(BuilderContext& ctx);
bool BuildFctid(BuilderContext& ctx);
bool BuildFctidz(BuilderContext& ctx);
bool BuildFctiw(BuilderContext& ctx);
bool BuildFctiwz(BuilderContext& ctx);
bool BuildFrsp(BuilderContext& ctx);

bool BuildFcmpu(BuilderContext& ctx);
bool BuildFcmpo(BuilderContext& ctx);

bool BuildFadd(BuilderContext& ctx);
bool BuildFadds(BuilderContext& ctx);

bool BuildFsub(BuilderContext& ctx);
bool BuildFsubs(BuilderContext& ctx);

bool BuildFmul(BuilderContext& ctx);
bool BuildFmuls(BuilderContext& ctx);

bool BuildFdiv(BuilderContext& ctx);
bool BuildFdivs(BuilderContext& ctx);

bool BuildFmadd(BuilderContext& ctx);
bool BuildFmadds(BuilderContext& ctx);
bool BuildFmsub(BuilderContext& ctx);
bool BuildFmsubs(BuilderContext& ctx);
bool BuildFnmadd(BuilderContext& ctx);
bool BuildFnmadds(BuilderContext& ctx);
bool BuildFnmsub(BuilderContext& ctx);
bool BuildFnmsubs(BuilderContext& ctx);

bool BuildFres(BuilderContext& ctx);
bool BuildFrsqrte(BuilderContext& ctx);
bool BuildFsqrt(BuilderContext& ctx);
bool BuildFsqrts(BuilderContext& ctx);

bool BuildFsel(BuilderContext& ctx);

bool BuildLi(BuilderContext& ctx);
bool BuildLis(BuilderContext& ctx);

bool BuildLbz(BuilderContext& ctx);
bool BuildLbzu(BuilderContext& ctx);
bool BuildLbzx(BuilderContext& ctx);
bool BuildLbzux(BuilderContext& ctx);

bool BuildLha(BuilderContext& ctx);
bool BuildLhau(BuilderContext& ctx);
bool BuildLhaux(BuilderContext& ctx);
bool BuildLhax(BuilderContext& ctx);
bool BuildLhbrx(BuilderContext& ctx);
bool BuildLhz(BuilderContext& ctx);
bool BuildLhzu(BuilderContext& ctx);
bool BuildLhzux(BuilderContext& ctx);
bool BuildLhzx(BuilderContext& ctx);

bool BuildLwa(BuilderContext& ctx);
bool BuildLwaux(BuilderContext& ctx);
bool BuildLwax(BuilderContext& ctx);
bool BuildLwbrx(BuilderContext& ctx);
bool BuildLwz(BuilderContext& ctx);
bool BuildLwzu(BuilderContext& ctx);
bool BuildLwzux(BuilderContext& ctx);
bool BuildLwzx(BuilderContext& ctx);

bool BuildLd(BuilderContext& ctx);
bool BuildLdu(BuilderContext& ctx);
bool BuildLdx(BuilderContext& ctx);
bool BuildLdux(BuilderContext& ctx);

bool BuildLwarx(BuilderContext& ctx);
bool BuildLdarx(BuilderContext& ctx);

bool BuildLfd(BuilderContext& ctx);
bool BuildLfdu(BuilderContext& ctx);
bool BuildLfdux(BuilderContext& ctx);
bool BuildLfdx(BuilderContext& ctx);
bool BuildLfs(BuilderContext& ctx);
bool BuildLfsu(BuilderContext& ctx);
bool BuildLfsux(BuilderContext& ctx);
bool BuildLfsx(BuilderContext& ctx);

bool BuildStb(BuilderContext& ctx);
bool BuildStbu(BuilderContext& ctx);
bool BuildStbx(BuilderContext& ctx);
bool BuildStbux(BuilderContext& ctx);

bool BuildSth(BuilderContext& ctx);
bool BuildSthbrx(BuilderContext& ctx);
bool BuildSthu(BuilderContext& ctx);
bool BuildSthux(BuilderContext& ctx);
bool BuildSthx(BuilderContext& ctx);

bool BuildStw(BuilderContext& ctx);
bool BuildStwu(BuilderContext& ctx);
bool BuildStwux(BuilderContext& ctx);
bool BuildStwx(BuilderContext& ctx);
bool BuildStwbrx(BuilderContext& ctx);
bool BuildStmw(BuilderContext& ctx);

bool BuildStwcx(BuilderContext& ctx);
bool BuildStdcx(BuilderContext& ctx);

bool BuildStd(BuilderContext& ctx);
bool BuildStdu(BuilderContext& ctx);
bool BuildStdx(BuilderContext& ctx);
bool BuildStdux(BuilderContext& ctx);

bool BuildStfd(BuilderContext& ctx);
bool BuildStfdu(BuilderContext& ctx);
bool BuildStfdux(BuilderContext& ctx);
bool BuildStfdx(BuilderContext& ctx);
bool BuildStfiwx(BuilderContext& ctx);
bool BuildSTFS(BuilderContext& ctx);
bool BuildStfsu(BuilderContext& ctx);
bool BuildStfsux(BuilderContext& ctx);
bool BuildStfsx(BuilderContext& ctx);

bool BuildLvx(BuilderContext& ctx);
bool BuildLvlx(BuilderContext& ctx);
bool BuildLvrx(BuilderContext& ctx);
bool BuildLvsl(BuilderContext& ctx);
bool BuildLvsr(BuilderContext& ctx);

bool BuildStvebx(BuilderContext& ctx);
bool BuildStvehx(BuilderContext& ctx);
bool BuildStvewx(BuilderContext& ctx);
bool BuildStvlx(BuilderContext& ctx);
bool BuildStvrx(BuilderContext& ctx);
bool BuildStvx(BuilderContext& ctx);

bool BuildNop(BuilderContext& ctx);
bool BuildAttn(BuilderContext& ctx);
bool BuildSync(BuilderContext& ctx);
bool BuildLwsync(BuilderContext& ctx);
bool BuildEieio(BuilderContext& ctx);
bool BuildDb16cyc(BuilderContext& ctx);
bool BuildCctpl(BuilderContext& ctx);
bool BuildCctpm(BuilderContext& ctx);
bool BuildCctph(BuilderContext& ctx);

bool BuildTwi(BuilderContext& ctx);
bool BuildTdi(BuilderContext& ctx);
bool BuildTw(BuilderContext& ctx);
bool BuildTd(BuilderContext& ctx);

bool BuildDcbf(BuilderContext& ctx);
bool BuildDcbt(BuilderContext& ctx);
bool BuildDcbtst(BuilderContext& ctx);
bool BuildDcbz(BuilderContext& ctx);
bool BuildDcbzl(BuilderContext& ctx);
bool BuildDcbst(BuilderContext& ctx);

bool BuildMr(BuilderContext& ctx);

bool BuildMcrf(BuilderContext& ctx);

bool BuildMfctr(BuilderContext& ctx);
bool BuildMfcr(BuilderContext& ctx);
bool BuildMfxer(BuilderContext& ctx);
bool BuildMfocrf(BuilderContext& ctx);
bool BuildMflr(BuilderContext& ctx);
bool BuildMfmsr(BuilderContext& ctx);
bool BuildMffs(BuilderContext& ctx);
bool BuildMftb(BuilderContext& ctx);
bool BuildMftbu(BuilderContext& ctx);

bool BuildMtcr(BuilderContext& ctx);
bool BuildMtcrf(BuilderContext& ctx);
bool BuildMtctr(BuilderContext& ctx);
bool BuildMtlr(BuilderContext& ctx);
bool BuildMtmsrd(BuilderContext& ctx);
bool BuildMtfsf(BuilderContext& ctx);
bool BuildMtxer(BuilderContext& ctx);

bool BuildClrldi(BuilderContext& ctx);

bool BuildVaddfp(BuilderContext& ctx);
bool BuildVsubfp(BuilderContext& ctx);
bool BuildVmulfp128(BuilderContext& ctx);
bool BuildVmaddfp(BuilderContext& ctx);
bool BuildVnmsubfp(BuilderContext& ctx);
bool BuildVmaxfp(BuilderContext& ctx);
bool BuildVminfp(BuilderContext& ctx);
bool BuildVrefp(BuilderContext& ctx);
bool BuildVrsqrtefp(BuilderContext& ctx);
bool BuildVexptefp(BuilderContext& ctx);
bool BuildVlogefp(BuilderContext& ctx);

bool BuildVmsum3fp128(BuilderContext& ctx);
bool BuildVmsum4fp128(BuilderContext& ctx);

bool BuildVrfim(BuilderContext& ctx);
bool BuildVrfin(BuilderContext& ctx);
bool BuildVrfip(BuilderContext& ctx);
bool BuildVrfiz(BuilderContext& ctx);

bool BuildVaddsbs(BuilderContext& ctx);
bool BuildVaddshs(BuilderContext& ctx);
bool BuildVaddsws(BuilderContext& ctx);
bool BuildVaddubm(BuilderContext& ctx);
bool BuildVaddubs(BuilderContext& ctx);
bool BuildVadduhm(BuilderContext& ctx);
bool BuildVadduwm(BuilderContext& ctx);
bool BuildVadduws(BuilderContext& ctx);
bool BuildVadduhs(BuilderContext& ctx);
bool BuildVsubsbs(BuilderContext& ctx);
bool BuildVsubshs(BuilderContext& ctx);
bool BuildVsubsws(BuilderContext& ctx);
bool BuildVsububm(BuilderContext& ctx);
bool BuildVsububs(BuilderContext& ctx);
bool BuildVsubuws(BuilderContext& ctx);
bool BuildVsubuhs(BuilderContext& ctx);
bool BuildVsubuhm(BuilderContext& ctx);
bool BuildVsubuwm(BuilderContext& ctx);
bool BuildVmaxsh(BuilderContext& ctx);
bool BuildVmaxsb(BuilderContext& ctx);
bool BuildVmaxsw(BuilderContext& ctx);
bool BuildVmaxuh(BuilderContext& ctx);
bool BuildVminsh(BuilderContext& ctx);
bool BuildVminsb(BuilderContext& ctx);
bool BuildVminsw(BuilderContext& ctx);
bool BuildVminuh(BuilderContext& ctx);
bool BuildVminuw(BuilderContext& ctx);
bool BuildVmaxub(BuilderContext& ctx);
bool BuildVminub(BuilderContext& ctx);

bool BuildVavgsb(BuilderContext& ctx);
bool BuildVavgsh(BuilderContext& ctx);
bool BuildVavgsw(BuilderContext& ctx);
bool BuildVavgub(BuilderContext& ctx);
bool BuildVavguh(BuilderContext& ctx);

bool BuildVand(BuilderContext& ctx);
bool BuildVandc(BuilderContext& ctx);
bool BuildVandc128(BuilderContext& ctx);
bool BuildVor(BuilderContext& ctx);
bool BuildVxor(BuilderContext& ctx);
bool BuildVnor(BuilderContext& ctx);
bool BuildVsel(BuilderContext& ctx);

bool BuildVcmpbfp(BuilderContext& ctx);
bool BuildVcmpeqfp(BuilderContext& ctx);
bool BuildVcmpequb(BuilderContext& ctx);
bool BuildVcmpequh(BuilderContext& ctx);
bool BuildVcmpequw(BuilderContext& ctx);
bool BuildVcmpgefp(BuilderContext& ctx);
bool BuildVcmpgtfp(BuilderContext& ctx);
bool BuildVcmpgtub(BuilderContext& ctx);
bool BuildVcmpgtuh(BuilderContext& ctx);
bool BuildVcmpgtuw(BuilderContext& ctx);
bool BuildVcmpgtsb(BuilderContext& ctx);
bool BuildVcmpgtsh(BuilderContext& ctx);
bool BuildVcmpgtsw(BuilderContext& ctx);

bool BuildMtvscr(BuilderContext& ctx);
bool BuildMfvscr(BuilderContext& ctx);
bool BuildVctsxs(BuilderContext& ctx);
bool BuildVctuxs(BuilderContext& ctx);
bool BuildVcfsx(BuilderContext& ctx);
bool BuildVcfux(BuilderContext& ctx);

bool BuildVmrghb(BuilderContext& ctx);
bool BuildVmrghh(BuilderContext& ctx);
bool BuildVmrghw(BuilderContext& ctx);
bool BuildVmrglb(BuilderContext& ctx);
bool BuildVmrglh(BuilderContext& ctx);
bool BuildVmrglw(BuilderContext& ctx);

bool BuildVperm(BuilderContext& ctx);
bool BuildVpermwi128(BuilderContext& ctx);
bool BuildVrlimi128(BuilderContext& ctx);

bool BuildVsl(BuilderContext& ctx);
bool BuildVslb(BuilderContext& ctx);
bool BuildVslh(BuilderContext& ctx);
bool BuildVsldoi(BuilderContext& ctx);
bool BuildVslw(BuilderContext& ctx);
bool BuildVslo(BuilderContext& ctx);
bool BuildVsr(BuilderContext& ctx);
bool BuildVsrh(BuilderContext& ctx);
bool BuildVsrb(BuilderContext& ctx);
bool BuildVsrab(BuilderContext& ctx);
bool BuildVsrah(BuilderContext& ctx);
bool BuildVsraw(BuilderContext& ctx);
bool BuildVsrw(BuilderContext& ctx);
bool BuildVsro(BuilderContext& ctx);
bool BuildVrlh(BuilderContext& ctx);
bool BuildVrlw(BuilderContext& ctx);

bool BuildVspltb(BuilderContext& ctx);
bool BuildVsplth(BuilderContext& ctx);
bool BuildVspltisb(BuilderContext& ctx);
bool BuildVspltish(BuilderContext& ctx);
bool BuildVspltisw(BuilderContext& ctx);
bool BuildVspltw(BuilderContext& ctx);

bool BuildVpkpx(BuilderContext& ctx);
bool BuildVpkuhum(BuilderContext& ctx);
bool BuildVpkuhus(BuilderContext& ctx);
bool BuildVpkuwum(BuilderContext& ctx);
bool BuildVpkuwus(BuilderContext& ctx);
bool BuildVpkshss(BuilderContext& ctx);
bool BuildVpkshus(BuilderContext& ctx);
bool BuildVpkswss(BuilderContext& ctx);
bool BuildVpkswus(BuilderContext& ctx);
bool BuildVpkd3d128(BuilderContext& ctx);

bool BuildVupkd3d128(BuilderContext& ctx);
bool BuildVupkhsb(BuilderContext& ctx);
bool BuildVupkhsh(BuilderContext& ctx);
bool BuildVupklsb(BuilderContext& ctx);
bool BuildVupklsh(BuilderContext& ctx);

}  // namespace rex::codegen
