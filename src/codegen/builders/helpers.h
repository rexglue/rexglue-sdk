/**
 * @file        codegen/builders/helpers.h
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

#include <dis-asm.h>
#include <ppc-inst.h>

namespace rex::codegen {

/**
 * Compute a 64-bit mask for PPC rotate/mask instructions.
 *
 * @param mstart Starting bit position (0-63)
 * @param mstop Ending bit position (0-63)
 * @return 64-bit mask with bits set between mstart and mstop
 */
inline u64 ComputeMask(u32 mstart, u32 mstop) {
  mstart &= 0x3F;
  mstop &= 0x3F;
  u64 value = (UINT64_MAX >> mstart) ^ ((mstop >= 63) ? 0 : UINT64_MAX >> (mstop + 1));
  return mstart <= mstop ? value : ~value;
}

/**
 * Returns the CRRegister member name of a BI field bit.
 *
 * @param bi BI field bit index, 0 to 3.
 * @return "lt", "gt", "eq" or "so".
 */
inline const char* GetCRBitName(u32 bi) {
  static constexpr const char* kNames[] = {"lt", "gt", "eq", "so"};
  return kNames[bi & 3];
}

/**
 * Reports whether the current instruction is a record form (has '.' suffix).
 *
 * Rc forms update CR0 from the result.
 *
 * @param insn The ppc_insn being processed
 * @return True when the instruction name contains '.' (record form)
 */
inline bool IsRecordForm(const ppc_insn& insn) {
  return std::strchr(insn.opcode->name, '.') != nullptr;
}

/**
 * Emits the CR0 comparison for Rc forms.
 *
 * Rc forms (the '.' suffix, such as add. and and.)
 * update CR0 based on the result compared to zero.
 *   CR0[LT] = result < 0
 *   CR0[GT] = result > 0
 *   CR0[EQ] = result == 0
 *   CR0[SO] = XER[SO]
 *
 * @param ctx The builder context containing the instruction being processed
 */
inline void EmitRecordFormCompare(BuilderContext& ctx) {
  if (IsRecordForm(ctx.insn)) {
    ctx.Println("\t{}.Compare<i32>({}.s32, 0, {});", ctx.CR(0), ctx.R(ctx.insn.operands[0]),
                ctx.XER());
  }
}

/**
 * Emit a CR bit operation `crD = crA <op> crB`.
 *
 * CR bit operations work on individual CR bits (0-31). This helper does the following.
 * - Maps bit indices to CR field (0-7) and field bit (0-3)
 * - Emits code to access CR fields by bit name
 *
 * @param ctx The builder context
 * @param op The operation symbol as a string (e.g., "|", "&", "^")
 * @param invert_a If true, invert the value of crA before the operation
 * @param invert_b If true, invert the value of crB before the operation
 * @param invert_result If true, invert the final result before storing in crD
 */
inline void EmitCRBitOperation(BuilderContext& ctx, std::string_view op, bool invert_a = false,
                               bool invert_b = false, bool invert_result = false) {
  u32 cr_d = ctx.insn.operands[0];
  u32 cr_a = ctx.insn.operands[1];
  u32 cr_b = ctx.insn.operands[2];

  u32 cr_field_d = cr_d / 4;
  u32 cr_bit_d = cr_d % 4;
  u32 cr_field_a = cr_a / 4;
  u32 cr_bit_a = cr_a % 4;
  u32 cr_field_b = cr_b / 4;
  u32 cr_bit_b = cr_b % 4;

  std::string a_expr = std::format("{}.{}", ctx.CR(cr_field_a), GetCRBitName(cr_bit_a));

  std::string b_expr = std::format("{}.{}", ctx.CR(cr_field_b), GetCRBitName(cr_bit_b));

  if (invert_a)
    a_expr = "!(" + a_expr + ")";
  if (invert_b)
    b_expr = "!(" + b_expr + ")";

  std::string expr = std::format("{} {} {}", a_expr, op, b_expr);

  if (invert_result)
    expr = "!(" + expr + ")";

  ctx.Println("\t{}.{} = {};", ctx.CR(cr_field_d), GetCRBitName(cr_bit_d), expr);
}

/**
 * Emit register-to-register comparison.
 *
 * Pattern `crD.compare<T>(rA.field, rB.field, XER)`
 * Used by cmpd, cmpld, cmplw, cmpw
 *
 * @param ctx The builder context
 * @param type_name The comparison type (e.g., "i64", "u32")
 * @param field The register field accessor (e.g., "s64", "u32")
 */
inline void EmitCompareRegister(BuilderContext& ctx, const char* type_name, const char* field) {
  ctx.Println("\t{}.Compare<{}>({}.{}, {}.{}, {});", ctx.CR(ctx.insn.operands[0]), type_name,
              ctx.R(ctx.insn.operands[1]), field, ctx.R(ctx.insn.operands[2]), field, ctx.XER());
}

/**
 * Emit register-to-immediate comparison.
 *
 * Pattern `crD.compare<T>(rA.field, imm, XER)`
 * Used by cmpdi, cmpldi, cmplwi, cmpwi
 *
 * @param ctx The builder context
 * @param type_name The comparison type (e.g., "i64", "u32")
 * @param field The register field accessor (e.g., "s64", "u32")
 * @param sign_extend If true, sign-extend the immediate via static_cast<i32>
 */
inline void EmitCompareImmediate(BuilderContext& ctx, const char* type_name, const char* field,
                                 bool sign_extend) {
  if (sign_extend) {
    ctx.Println("\t{}.Compare<{}>({}.{}, {}, {});", ctx.CR(ctx.insn.operands[0]), type_name,
                ctx.R(ctx.insn.operands[1]), field, static_cast<i32>(ctx.insn.operands[2]),
                ctx.XER());
  } else {
    ctx.Println("\t{}.Compare<{}>({}.{}, {}, {});", ctx.CR(ctx.insn.operands[0]), type_name,
                ctx.R(ctx.insn.operands[1]), field, ctx.insn.operands[2], ctx.XER());
  }
}

/**
 * Emit D-form load with update instruction.
 *
 * Pattern `EA = (rA) + d; rD = MEM[EA]; rA = EA`
 * Used by lbzu, lwzu, ldu and others
 *
 * @param ctx The builder context
 * @param load_macro The REX_LOAD_* macro to use (e.g., "REX_LOAD_U8")
 */
inline void EmitLoadWithUpdate(BuilderContext& ctx, const char* load_macro) {
  ctx.Println("\t{} = {} + {}.u32;", ctx.EA(), static_cast<i32>(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.u64 = {}({});", ctx.R(ctx.insn.operands[0]), ctx.MMIOMacro(load_macro),
              ctx.EA());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[2]), ctx.EA());
}

/**
 * Emit X-form load with update instruction.
 *
 * Pattern `EA = rA + rB; rD = MEM[EA]; rA = EA`
 * Used by lbzux, lhzux, lwzux, ldux
 *
 * @param ctx The builder context
 * @param load_macro The REX_LOAD_* macro to use (e.g., "REX_LOAD_U8")
 */
inline void EmitLoadXFormWithUpdate(BuilderContext& ctx, const char* load_macro) {
  ctx.Println("\t{} = {}.u32 + {}.u32;", ctx.EA(), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.u64 = {}({});", ctx.R(ctx.insn.operands[0]), ctx.MMIOMacro(load_macro),
              ctx.EA());
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[1]), ctx.EA());
}

/**
 * Emit D-form store with update instruction.
 *
 * Pattern `EA = (rA) + d; MEM[EA] = rS; rA = EA`
 * Used by stbu, stwu, stdu and others
 *
 * @param ctx The builder context
 * @param store_macro The REX_STORE_* macro to use (e.g., "REX_STORE_U8")
 * @param field The register field to store (e.g., "u8", "u32", "u64")
 */
inline void EmitStoreWithUpdate(BuilderContext& ctx, const char* store_macro, const char* field) {
  ctx.Println("\t{} = {} + {}.u32;", ctx.EA(), static_cast<i32>(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}({}, {}.{});", ctx.MMIOMacro(store_macro), ctx.EA(), ctx.R(ctx.insn.operands[0]),
              field);
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[2]), ctx.EA());
}

/**
 * Emit X-form store with update instruction.
 *
 * Pattern `EA = rA + rB; MEM[EA] = rS; rA = EA`
 * Used by stbux, sthux, stwux, stdux
 *
 * @param ctx The builder context
 * @param store_macro The REX_STORE_* macro (e.g., "REX_STORE_U8")
 * @param field The register field to store (e.g., "u8", "u32", "u64")
 */
inline void EmitStoreXFormWithUpdate(BuilderContext& ctx, const char* store_macro,
                                     const char* field) {
  ctx.Println("\t{} = {}.u32 + {}.u32;", ctx.EA(), ctx.R(ctx.insn.operands[1]),
              ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}({}, {}.{});", ctx.MMIOMacro(store_macro), ctx.EA(), ctx.R(ctx.insn.operands[0]),
              field);
  ctx.Println("\t{}.u32 = {};", ctx.R(ctx.insn.operands[1]), ctx.EA());
}

/**
 * Emit atomic load-and-reserve instruction (lwarx/ldarx pattern).
 *
 * Pattern `EA = rA + rB; reserved = *(T*)REX_RAW_ADDR(EA); rD = bswap(reserved)`
 *
 * @param ctx The builder context
 * @param ptr_type The pointer type (e.g., "u32", "u64")
 * @param bswap_func The byte-swap builtin (e.g., "__builtin_bswap32")
 * @param reserved_field The reserved register field (e.g., "u32", "u64")
 */
inline void EmitAtomicLoadReserve(BuilderContext& ctx, const char* ptr_type, const char* bswap_func,
                                  const char* reserved_field) {
  ctx.Print("\t{} = ", ctx.EA());
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32;", ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.{} = *({}*)REX_RAW_ADDR({});", ctx.Reserved(), reserved_field, ptr_type,
              ctx.EA());
  ctx.Println("\t{}.u64 = {}({}.{});", ctx.R(ctx.insn.operands[0]), bswap_func, ctx.Reserved(),
              reserved_field);
}

/**
 * Emit atomic store-conditional instruction (stwcx./stdcx. pattern).
 *
 * Pattern `EA = rA + rB; cr0 = CAS(EA, reserved, bswap(rS))`
 *
 * @param ctx The builder context
 * @param ptr_type The pointer type (e.g., "u32", "u64")
 * @param bswap_func The byte-swap builtin (e.g., "__builtin_bswap32")
 * @param field The register field (e.g., "u32", "u64")
 */
inline void EmitAtomicStoreConditional(BuilderContext& ctx, const char* ptr_type,
                                       const char* bswap_func, const char* field) {
  ctx.Print("\t{} = ", ctx.EA());
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32;", ctx.R(ctx.insn.operands[2]));
  ctx.Println("\t{}.lt = 0;", ctx.CR(0));
  ctx.Println("\t{}.gt = 0;", ctx.CR(0));
  ctx.Println(
      "\t{}.eq = reinterpret_cast<std::atomic<{}>*>(REX_RAW_ADDR({}))"
      "->compare_exchange_strong({}.{}, {}({}.{}), "
      "std::memory_order_acq_rel, std::memory_order_acquire);",
      ctx.CR(0), ptr_type, ctx.EA(), ctx.Reserved(), field, bswap_func, ctx.R(ctx.insn.operands[0]),
      field);
}

/**
 * Emit D-form sign-extending load instruction.
 *
 * Pattern `rD = sign_extend(LOAD(rA + d))`
 * Used by lha, lwa (halfword/word algebraic loads)
 *
 * @param ctx The builder context
 * @param cast_type The cast for sign extension (e.g., "i16", "i32")
 * @param load_macro The REX_LOAD_* macro to use
 */
inline void EmitSignExtendLoadDForm(BuilderContext& ctx, const char* cast_type,
                                    const char* load_macro) {
  ctx.Print("\t{}.s64 = {}({}(", ctx.R(ctx.insn.operands[0]), cast_type, ctx.MMIOMacro(load_macro));
  if (ctx.insn.operands[2] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[2]));
  ctx.Println("{}));", static_cast<i32>(ctx.insn.operands[1]));
}

/**
 * Emit X-form sign-extending load instruction.
 *
 * Pattern `rD = sign_extend(LOAD(rA + rB))`
 * Used by lhax, lwax (halfword/word algebraic indexed loads)
 *
 * @param ctx The builder context
 * @param cast_type The cast for sign extension (e.g., "i16", "i32")
 * @param load_macro The REX_LOAD_* macro to use
 */
inline void EmitSignExtendLoadXForm(BuilderContext& ctx, const char* cast_type,
                                    const char* load_macro) {
  ctx.Print("\t{}.s64 = {}({}(", ctx.R(ctx.insn.operands[0]), cast_type, ctx.MMIOMacro(load_macro));
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32));", ctx.R(ctx.insn.operands[2]));
}

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
inline void EmitBranchWithBoundsCheck(BuilderContext& ctx, u32 target, std::string_view condition,
                                      std::string_view instr_name) {
  if (target < ctx.fn.GetBase() || target >= ctx.fn.GetEnd()) {
    REXLOG_WARN(codegen, "{} at {:X} branches outside function to {:X}", instr_name, ctx.base,
                target);
    ctx.Println("\tif ({}) {{ /* branch to 0x{:X} outside function */ return; }}", condition,
                target);
  } else {
    ctx.Println("\tif ({}) goto loc_{:X};", condition, target);
  }
}

/**
 * Emit aligned or unaligned vector effective address calculation to ea.
 *
 * Pattern `ea = (opt_rA + rB) [& ~align_mask]`
 * Uses operands[1] as optional base register and operands[2] as index register.
 *
 * @param ctx The builder context
 * @param align_mask Alignment mask string (e.g., "0xF"), or nullptr for no alignment
 */
inline void EmitVectorEA(BuilderContext& ctx, const char* align_mask = nullptr) {
  if (align_mask)
    ctx.Print("\t{} = (", ctx.EA());
  else
    ctx.Print("\t{} = ", ctx.EA());
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  if (align_mask)
    ctx.Println("{}.u32) & ~{};", ctx.R(ctx.insn.operands[2]), align_mask);
  else
    ctx.Println("{}.u32;", ctx.R(ctx.insn.operands[2]));
}

/**
 * Emit unaligned vector effective address calculation to temp.
 *
 * Pattern `temp.u32 = opt_rA + rB`
 * Uses operands[1] as optional base register and operands[2] as index register.
 *
 * @param ctx The builder context
 */
inline void EmitVectorTempEA(BuilderContext& ctx) {
  ctx.Print("\t{}.u32 = ", ctx.Temp());
  if (ctx.insn.operands[1] != 0)
    ctx.Print("{}.u32 + ", ctx.R(ctx.insn.operands[1]));
  ctx.Println("{}.u32;", ctx.R(ctx.insn.operands[2]));
}

/**
 * Emit a PPC trap instruction `if (<condition>) ppc_trap(ctx, base, 0);`.
 *
 * The TO field (instruction bits 21-25) is a 5-bit mask of the trap conditions in the order
 * signed lt, signed gt, eq, unsigned lt, unsigned gt. It is read from the instruction word so
 * the generic (tw TO,rA,rB) and simplified (tweq rA,rB) forms share one builder.
 *
 * @param ctx      The builder context
 * @param to       5-bit TO field
 * @param a_signed  First operand, signed (e.g., "ctx.r3.s32")
 * @param a_unsigned First operand, unsigned (e.g., "ctx.r3.u32")
 * @param bSigned  Second operand, signed (e.g., "ctx.r4.s32" or "-1")
 * @param bUnsigned Second operand, unsigned (e.g., "ctx.r4.u32" or "4294967295u")
 */
inline void EmitTrap(BuilderContext& ctx, u32 to, const std::string& a_signed,
                     const std::string& a_unsigned, const std::string& bSigned,
                     const std::string& bUnsigned) {
  if (to == 0)
    return;
  if (to == 0x1F) {
    ctx.Println("\tppc_trap(ctx, base, 0);");
    return;
  }

  std::string cond;
  auto add = [&](std::string_view c) {
    if (!cond.empty())
      cond += " || ";
    cond += c;
  };
  if (to & 0x10)
    add(std::format("{} < {}", a_signed, bSigned));
  if (to & 0x08)
    add(std::format("{} > {}", a_signed, bSigned));
  if (to & 0x04)
    add(std::format("{} == {}", a_signed, bSigned));
  if (to & 0x02)
    add(std::format("{} < {}", a_unsigned, bUnsigned));
  if (to & 0x01)
    add(std::format("{} > {}", a_unsigned, bUnsigned));

  ctx.Println("\tif ({}) ppc_trap(ctx, base, 0);", cond);
}

}  // namespace rex::codegen
