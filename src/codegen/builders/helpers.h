/**
 * @file        rexcodegen/internal/helpers.h
 * @brief       Recompiler helper utilities
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "builder_context.h"

#include <cstring>

#include <rex/codegen/function_scanner.h>
#include <rex/logging.h>

#include "../codegen_logging.h"

#include <dis-asm.h>
#include <ppc-inst.h>

namespace rex::codegen {

/**
 * Compute a 64-bit mask for PPC rotate/mask instructions.
 * @param mstart Starting bit position (0-63)
 * @param mstop Ending bit position (0-63)
 * @return 64-bit mask with bits set between mstart and mstop
 */
inline uint64_t compute_mask(uint32_t mstart, uint32_t mstop) {
  mstart &= 0x3F;
  mstop &= 0x3F;
  uint64_t value = (UINT64_MAX >> mstart) ^ ((mstop >= 63) ? 0 : UINT64_MAX >> (mstop + 1));
  return mstart <= mstop ? value : ~value;
}

//=============================================================================
// CR Bit Helpers
//=============================================================================

/// Map PPC BI field bit index (0-3) to CRRegister member name.
inline const char* crBitName(uint32_t bi) {
  static constexpr const char* names[] = {"lt", "gt", "eq", "so"};
  return names[bi & 3];
}

//=============================================================================
// Record-Form Helpers
//=============================================================================

/**
 * Check if the current instruction is a record form (has '.' suffix).
 *
 * Record-form instructions update CR0 based on the result.
 *
 * @param insn The ppc_insn being processed
 * @return true if the instruction name contains '.' (record form)
 */
inline bool isRecordForm(const ppc_insn& insn) {
  return std::strchr(insn.opcode->name, '.') != nullptr;
}

/**
 * Emit CR0 comparison for record-form instructions.
 *
 * Record-form instructions (those with '.' suffix like add., and., etc.)
 * update CR0 based on the result compared to zero:
 *   CR0[LT] = result < 0
 *   CR0[GT] = result > 0
 *   CR0[EQ] = result == 0
 *   CR0[SO] = XER[SO]
 *
 * @param ctx The builder context containing the instruction being processed
 */
inline void emitRecordFormCompare(BuilderContext& ctx) {
  if (isRecordForm(ctx.insn)) {
    ctx.println("\t{}.compare<int32_t>({}.s32, 0, {});", ctx.cr(0), ctx.r(ctx.insn.operands[0]),
                ctx.xer());
  }
}

/**
 * Emit a CR bit operation: crD = crA <op> crB
 *
 * CR bit operations work on individual CR bits (0-31). This helper:
 * - Maps bit indices to CR field (0-7) and field bit (0-3)
 * - Emits code to access CR fields by bit name
 *
 * @param ctx The builder context
 * @param op The operation symbol as a string (e.g., "|", "&", "^")
 * @param invertA If true, invert the value of crA before the operation
 * @param invertB If true, invert the value of crB before the operation
 * @param invertResult If true, invert the final result before storing in crD
 */
inline void emitCRBitOperation(BuilderContext& ctx, std::string_view op, bool invertA = false,
                               bool invertB = false, bool invertResult = false) {
  uint32_t crD = ctx.insn.operands[0];
  uint32_t crA = ctx.insn.operands[1];
  uint32_t crB = ctx.insn.operands[2];

  uint32_t crField_D = crD / 4;
  uint32_t crBit_D = crD % 4;
  uint32_t crField_A = crA / 4;
  uint32_t crBit_A = crA % 4;
  uint32_t crField_B = crB / 4;
  uint32_t crBit_B = crB % 4;

  std::string aExpr = fmt::format("{}.{}", ctx.cr(crField_A), crBitName(crBit_A));

  std::string bExpr = fmt::format("{}.{}", ctx.cr(crField_B), crBitName(crBit_B));

  if (invertA)
    aExpr = "!(" + aExpr + ")";
  if (invertB)
    bExpr = "!(" + bExpr + ")";

  std::string expr = fmt::format("{} {} {}", aExpr, op, bExpr);

  if (invertResult)
    expr = "!(" + expr + ")";

  ctx.println("\t{}.{} = {};", ctx.cr(crField_D), crBitName(crBit_D), expr);
}

//=============================================================================
// Comparison Instruction Helpers
//=============================================================================

/**
 * Emit register-to-register comparison.
 *
 * Pattern: crD.compare<T>(rA.field, rB.field, XER)
 * Used by: cmpd, cmpld, cmplw, cmpw
 *
 * @param ctx The builder context
 * @param type_name The comparison type (e.g., "int64_t", "uint32_t")
 * @param field The register field accessor (e.g., "s64", "u32")
 */
inline void emitCompareRegister(BuilderContext& ctx, const char* type_name, const char* field) {
  ctx.println("\t{}.compare<{}>({}.{}, {}.{}, {});", ctx.cr(ctx.insn.operands[0]), type_name,
              ctx.r(ctx.insn.operands[1]), field, ctx.r(ctx.insn.operands[2]), field, ctx.xer());
}

/**
 * Emit register-to-immediate comparison.
 *
 * Pattern: crD.compare<T>(rA.field, imm, XER)
 * Used by: cmpdi, cmpldi, cmplwi, cmpwi
 *
 * @param ctx The builder context
 * @param type_name The comparison type (e.g., "int64_t", "uint32_t")
 * @param field The register field accessor (e.g., "s64", "u32")
 * @param sign_extend If true, sign-extend the immediate via static_cast<int32_t>
 */
inline void emitCompareImmediate(BuilderContext& ctx, const char* type_name, const char* field,
                                 bool sign_extend) {
  if (sign_extend) {
    ctx.println("\t{}.compare<{}>({}.{}, {}, {});", ctx.cr(ctx.insn.operands[0]), type_name,
                ctx.r(ctx.insn.operands[1]), field, static_cast<int32_t>(ctx.insn.operands[2]),
                ctx.xer());
  } else {
    ctx.println("\t{}.compare<{}>({}.{}, {}, {});", ctx.cr(ctx.insn.operands[0]), type_name,
                ctx.r(ctx.insn.operands[1]), field, ctx.insn.operands[2], ctx.xer());
  }
}

//=============================================================================
// Memory Operation Helpers
//=============================================================================

/**
 * Emit D-form load with update instruction.
 *
 * Pattern: EA = (rA) + d; rD = MEM[EA]; rA = EA
 * Used by: lbzu, lwzu, ldu, etc.
 *
 * @param ctx The builder context
 * @param load_macro The PPC_LOAD_* macro to use (e.g., "PPC_LOAD_U8")
 */
inline void emitLoadWithUpdate(BuilderContext& ctx, const char* load_macro) {
  // EA = displacement + rA
  ctx.println("\t{} = {} + {}.u32;", ctx.ea(), static_cast<int32_t>(ctx.insn.operands[1]),
              ctx.r(ctx.insn.operands[2]));
  // rD = MEM[EA]
  ctx.println("\t{}.u64 = {}({});", ctx.r(ctx.insn.operands[0]), load_macro, ctx.ea());
  // rA = EA (update)
  ctx.println("\t{}.u32 = {};", ctx.r(ctx.insn.operands[2]), ctx.ea());
}

/**
 * Emit X-form load with update instruction.
 *
 * Pattern: EA = rA + rB; rD = MEM[EA]; rA = EA
 * Used by: lbzux, lhzux, lwzux, ldux
 *
 * @param ctx The builder context
 * @param load_macro The PPC_LOAD_* macro to use (e.g., "PPC_LOAD_U8")
 */
inline void emitLoadXFormWithUpdate(BuilderContext& ctx, const char* load_macro) {
  ctx.println("\t{} = {}.u32 + {}.u32;", ctx.ea(), ctx.r(ctx.insn.operands[1]),
              ctx.r(ctx.insn.operands[2]));
  ctx.println("\t{}.u64 = {}({});", ctx.r(ctx.insn.operands[0]), load_macro, ctx.ea());
  ctx.println("\t{}.u32 = {};", ctx.r(ctx.insn.operands[1]), ctx.ea());
}

/**
 * Emit D-form store with update instruction.
 *
 * Pattern: EA = (rA) + d; MEM[EA] = rS; rA = EA
 * Used by: stbu, stwu, stdu, etc.
 *
 * @param ctx The builder context
 * @param store_macro The PPC_STORE_* macro to use (e.g., "PPC_STORE_U8")
 * @param field The register field to store (e.g., "u8", "u32", "u64")
 */
inline void emitStoreWithUpdate(BuilderContext& ctx, const char* store_macro, const char* field) {
  // EA = displacement + rA
  ctx.println("\t{} = {} + {}.u32;", ctx.ea(), static_cast<int32_t>(ctx.insn.operands[1]),
              ctx.r(ctx.insn.operands[2]));
  // MEM[EA] = rS
  ctx.println("\t{}({}, {}.{});", store_macro, ctx.ea(), ctx.r(ctx.insn.operands[0]), field);
  // rA = EA (update)
  ctx.println("\t{}.u32 = {};", ctx.r(ctx.insn.operands[2]), ctx.ea());
}

/**
 * Emit X-form store with update instruction.
 *
 * Pattern: EA = rA + rB; MEM[EA] = rS; rA = EA
 * Used by: stbux, sthux, stwux, stdux
 *
 * @param ctx The builder context
 * @param store_macro The PPC_STORE_* normal macro (e.g., "PPC_STORE_U8")
 * @param mmio_macro The PPC_MM_STORE_* MMIO macro (e.g., "PPC_MM_STORE_U8")
 * @param field The register field to store (e.g., "u8", "u32", "u64")
 */
inline void emitStoreXFormWithUpdate(BuilderContext& ctx, const char* store_macro,
                                     const char* mmio_macro, const char* field) {
  ctx.println("\t{} = {}.u32 + {}.u32;", ctx.ea(), ctx.r(ctx.insn.operands[1]),
              ctx.r(ctx.insn.operands[2]));
  ctx.println("\t{}({}, {}.{});", ctx.mmio_check_x_form() ? mmio_macro : store_macro, ctx.ea(),
              ctx.r(ctx.insn.operands[0]), field);
  ctx.println("\t{}.u32 = {};", ctx.r(ctx.insn.operands[1]), ctx.ea());
}

/**
 * Get the appropriate store macro based on MMIO context.
 *
 * @param ctx The builder context
 * @param normal_macro Normal store macro (e.g., "PPC_STORE_U32")
 * @param mmio_macro MMIO store macro (e.g., "PPC_MM_STORE_U32")
 * @return The appropriate macro string
 */
inline const char* getStoreMacro(BuilderContext& ctx, const char* normal_macro,
                                 const char* mmio_macro) {
  return ctx.mmio_check_d_form() ? mmio_macro : normal_macro;
}

//=============================================================================
// Atomic Operation Helpers
//=============================================================================

/**
 * Emit atomic load-and-reserve instruction (lwarx/ldarx pattern).
 *
 * Pattern: EA = rA + rB; reserved = *(T*)PPC_RAW_ADDR(EA); rD = bswap(reserved)
 *
 * @param ctx The builder context
 * @param ptr_type The pointer type (e.g., "uint32_t", "uint64_t")
 * @param bswap_func The byte-swap builtin (e.g., "__builtin_bswap32")
 * @param reserved_field The reserved register field (e.g., "u32", "u64")
 */
inline void emitAtomicLoadReserve(BuilderContext& ctx, const char* ptr_type, const char* bswap_func,
                                  const char* reserved_field) {
  ctx.print("\t{} = ", ctx.ea());
  if (ctx.insn.operands[1] != 0)
    ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[1]));
  ctx.println("{}.u32;", ctx.r(ctx.insn.operands[2]));
  ctx.println("\t{}.{} = *({}*)PPC_RAW_ADDR({});", ctx.reserved(), reserved_field, ptr_type,
              ctx.ea());
  ctx.println("\t{}.u64 = {}({}.{});", ctx.r(ctx.insn.operands[0]), bswap_func, ctx.reserved(),
              reserved_field);
}

/**
 * Emit atomic store-conditional instruction (stwcx./stdcx. pattern).
 *
 * Pattern: EA = rA + rB; cr0 = CAS(EA, reserved, bswap(rS))
 *
 * @param ctx The builder context
 * @param ptr_type The pointer type (e.g., "uint32_t", "uint64_t")
 * @param bswap_func The byte-swap builtin (e.g., "__builtin_bswap32")
 * @param field The register field (e.g., "s32", "s64")
 */
inline void emitAtomicStoreConditional(BuilderContext& ctx, const char* ptr_type,
                                       const char* bswap_func, const char* field) {
  ctx.print("\t{} = ", ctx.ea());
  if (ctx.insn.operands[1] != 0)
    ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[1]));
  ctx.println("{}.u32;", ctx.r(ctx.insn.operands[2]));
  ctx.println("\t{}.lt = 0;", ctx.cr(0));
  ctx.println("\t{}.gt = 0;", ctx.cr(0));
  ctx.println(
      "\t{}.eq = __sync_bool_compare_and_swap(reinterpret_cast<{}*>(PPC_RAW_ADDR({})), "
      "{}.{}, {}({}.{}));",
      ctx.cr(0), ptr_type, ctx.ea(), ctx.reserved(), field, bswap_func, ctx.r(ctx.insn.operands[0]),
      field);
  ctx.println("\t{}.so = {}.so;", ctx.cr(0), ctx.xer());
}

//=============================================================================
// Sign-Extending Load Helpers
//=============================================================================

/**
 * Emit D-form sign-extending load instruction.
 *
 * Pattern: rD = sign_extend(LOAD(rA + d))
 * Used by: lha, lwa (halfword/word algebraic loads)
 *
 * @param ctx The builder context
 * @param cast_type The cast for sign extension (e.g., "int16_t", "int32_t")
 * @param load_macro The PPC_LOAD_* macro to use
 */
inline void emitSignExtendLoadDForm(BuilderContext& ctx, const char* cast_type,
                                    const char* load_macro) {
  ctx.print("\t{}.s64 = {}({}(", ctx.r(ctx.insn.operands[0]), cast_type, load_macro);
  if (ctx.insn.operands[2] != 0)
    ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[2]));
  ctx.println("{}));", static_cast<int32_t>(ctx.insn.operands[1]));
}

/**
 * Emit X-form sign-extending load instruction.
 *
 * Pattern: rD = sign_extend(LOAD(rA + rB))
 * Used by: lhax, lwax (halfword/word algebraic indexed loads)
 *
 * @param ctx The builder context
 * @param cast_type The cast for sign extension (e.g., "int16_t", "int32_t")
 * @param load_macro The PPC_LOAD_* macro to use
 */
inline void emitSignExtendLoadXForm(BuilderContext& ctx, const char* cast_type,
                                    const char* load_macro) {
  ctx.print("\t{}.s64 = {}({}(", ctx.r(ctx.insn.operands[0]), cast_type, load_macro);
  if (ctx.insn.operands[1] != 0)
    ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[1]));
  ctx.println("{}.u32));", ctx.r(ctx.insn.operands[2]));
}

//=============================================================================
// MMIO Detection Helpers
//=============================================================================

/**
 * Check if an upper-16-bit immediate value corresponds to a known MMIO range.
 *
 * Xbox 360 hardware register ranges:
 * - GPU MMIO: 0x7FC80000-0x7FCFFFFF (upper bits: 0x7FC8-0x7FCF)
 * - XMA/APU MMIO: 0x7FEA0000-0x7FEAFFFF (upper bits: 0x7FEA)
 *
 * @param imm The upper 16 bits loaded by lis/oris
 * @return true if the value matches a known MMIO base address range
 */
inline bool isMMIOUpperBits(uint32_t imm) {
  return (imm >= 0x7FC8 && imm <= 0x7FCF) || imm == 0x7FEA;
}

//=============================================================================
// Branch Bounds-Checking Helper
//=============================================================================

/**
 * Emit a conditional branch with bounds checking.
 *
 * If the target is within the current function, emits a goto.
 * If outside, emits a warning and a return statement.
 *
 * @param ctx The builder context
 * @param target Target address of the branch
 * @param condition Pre-formatted condition expression (e.g., "ctr.u32 != 0")
 * @param instr_name Instruction mnemonic for the warning message
 */
inline void emitBranchWithBoundsCheck(BuilderContext& ctx, uint32_t target,
                                      std::string_view condition, std::string_view instr_name) {
  if (target < ctx.fn.base() || target >= ctx.fn.end()) {
    REXCODEGEN_WARN("{} at {:X} branches outside function to {:X}", instr_name, ctx.base, target);
    ctx.println("\tif ({}) {{ /* branch to 0x{:X} outside function */ return; }}", condition,
                target);
  } else {
    ctx.println("\tif ({}) goto loc_{:X};", condition, target);
  }
}

//=============================================================================
// Vector EA Calculation Helpers
//=============================================================================

/**
 * Emit aligned or unaligned vector effective address calculation to ea.
 *
 * Pattern: ea = (opt_rA + rB) [& ~align_mask]
 * Uses operands[1] as optional base register and operands[2] as index register.
 *
 * @param ctx The builder context
 * @param align_mask Alignment mask string (e.g., "0xF"), or nullptr for no alignment
 */
inline void emitVectorEA(BuilderContext& ctx, const char* align_mask = nullptr) {
  if (align_mask)
    ctx.print("\t{} = (", ctx.ea());
  else
    ctx.print("\t{} = ", ctx.ea());
  if (ctx.insn.operands[1] != 0)
    ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[1]));
  if (align_mask)
    ctx.println("{}.u32) & ~{};", ctx.r(ctx.insn.operands[2]), align_mask);
  else
    ctx.println("{}.u32;", ctx.r(ctx.insn.operands[2]));
}

/**
 * Emit unaligned vector effective address calculation to temp.
 *
 * Pattern: temp.u32 = opt_rA + rB
 * Uses operands[1] as optional base register and operands[2] as index register.
 *
 * @param ctx The builder context
 */
inline void emitVectorTempEA(BuilderContext& ctx) {
  ctx.print("\t{}.u32 = ", ctx.temp());
  if (ctx.insn.operands[1] != 0)
    ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[1]));
  ctx.println("{}.u32;", ctx.r(ctx.insn.operands[2]));
}

//=============================================================================
// Trap Instruction Helper
//=============================================================================

/**
 * Emit a PPC trap instruction: if (<condition>) ppc_trap(ctx, base, 0);
 *
 * @param to       5-bit TO field
 * @param aSigned  First operand, signed (e.g., "ctx.r3.s32")
 * @param aUnsigned First operand, unsigned (e.g., "ctx.r3.u32")
 * @param bSigned  Second operand, signed (e.g., "ctx.r4.s32" or "-1")
 * @param bUnsigned Second operand, unsigned (e.g., "ctx.r4.u32" or "4294967295u")
 */
inline void emitTrap(BuilderContext& ctx, uint32_t to, const std::string& aSigned,
                     const std::string& aUnsigned, const std::string& bSigned,
                     const std::string& bUnsigned) {
  if (to == 0)
    return;
  if (to == 0x1F) {
    ctx.println("\tppc_trap(ctx, base, 0);");
    return;
  }

  std::string cond;
  auto add = [&](std::string_view c) {
    if (!cond.empty())
      cond += " || ";
    cond += c;
  };
  if (to & 0x10)
    add(fmt::format("{} < {}", aSigned, bSigned));
  if (to & 0x08)
    add(fmt::format("{} > {}", aSigned, bSigned));
  if (to & 0x04)
    add(fmt::format("{} == {}", aSigned, bSigned));
  if (to & 0x02)
    add(fmt::format("{} < {}", aUnsigned, bUnsigned));
  if (to & 0x01)
    add(fmt::format("{} > {}", aUnsigned, bUnsigned));

  ctx.println("\tif ({}) ppc_trap(ctx, base, 0);", cond);
}

}  // namespace rex::codegen
