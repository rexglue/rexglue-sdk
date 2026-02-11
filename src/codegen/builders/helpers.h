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

#include "../builder_context.h"
#include <rex/codegen/recompiler.h>
#include <rex/codegen/recompiled_function.h>
#include <rex/logging.h>
#include <ppc-inst.h>
#include <dis-asm.h>
#include <cstring>

namespace rex::codegen {

/**
* Compute a 64-bit mask for PPC rotate/mask instructions.
* @param mstart Starting bit position (0-63)
* @param mstop Ending bit position (0-63)
* @return 64-bit mask with bits set between mstart and mstop
*/
inline uint64_t compute_mask(uint32_t mstart, uint32_t mstop)
{
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
    static constexpr const char* names[] = { "lt", "gt", "eq", "so" };
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
inline bool isRecordForm(const ppc_insn& insn)
{
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
inline void emitRecordFormCompare(BuilderContext& ctx)
{
    if (isRecordForm(ctx.insn))
    {
        ctx.println("\t{}.compare<int32_t>({}.s32, 0, {});",
            ctx.cr(0), ctx.r(ctx.insn.operands[0]), ctx.xer());
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
inline void emitLoadWithUpdate(BuilderContext& ctx, const char* load_macro)
{
    // EA = displacement + rA
    ctx.println("\t{} = {} + {}.u32;",
        ctx.ea(),
        static_cast<int32_t>(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    // rD = MEM[EA]
    ctx.println("\t{}.u64 = {}({});",
        ctx.r(ctx.insn.operands[0]), load_macro, ctx.ea());
    // rA = EA (update)
    ctx.println("\t{}.u32 = {};",
        ctx.r(ctx.insn.operands[2]), ctx.ea());
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
inline void emitStoreWithUpdate(BuilderContext& ctx, const char* store_macro, const char* field)
{
    // EA = displacement + rA
    ctx.println("\t{} = {} + {}.u32;",
        ctx.ea(),
        static_cast<int32_t>(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    // MEM[EA] = rS
    ctx.println("\t{}({}, {}.{});",
        store_macro, ctx.ea(), ctx.r(ctx.insn.operands[0]), field);
    // rA = EA (update)
    ctx.println("\t{}.u32 = {};",
        ctx.r(ctx.insn.operands[2]), ctx.ea());
}

/**
 * Get the appropriate store macro based on MMIO context.
 *
 * @param ctx The builder context
 * @param normal_macro Normal store macro (e.g., "PPC_STORE_U32")
 * @param mmio_macro MMIO store macro (e.g., "PPC_MM_STORE_U32")
 * @return The appropriate macro string
 */
inline const char* getStoreMacro(BuilderContext& ctx, const char* normal_macro, const char* mmio_macro)
{
    return ctx.mmio_check_d_form() ? mmio_macro : normal_macro;
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
inline void emitSignExtendLoadDForm(BuilderContext& ctx, const char* cast_type, const char* load_macro)
{
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
inline void emitSignExtendLoadXForm(BuilderContext& ctx, const char* cast_type, const char* load_macro)
{
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
inline bool isMMIOUpperBits(uint32_t imm)
{
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
    std::string_view condition, std::string_view instr_name)
{
    if (target < ctx.fn.base() || target >= ctx.fn.end()) {
        REXCODEGEN_WARN("{} at {:X} branches outside function to {:X}",
            instr_name, ctx.base, target);
        ctx.println("\tif ({}) {{ /* branch to 0x{:X} outside function */ return; }}",
            condition, target);
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
inline void emitVectorEA(BuilderContext& ctx, const char* align_mask = nullptr)
{
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
inline void emitVectorTempEA(BuilderContext& ctx)
{
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
inline void emitTrap(BuilderContext& ctx, uint32_t to,
    const std::string& aSigned, const std::string& aUnsigned,
    const std::string& bSigned, const std::string& bUnsigned)
{
    if (to == 0) return;
    if (to == 0x1F) { ctx.println("\tppc_trap(ctx, base, 0);"); return; }

    std::string cond;
    auto add = [&](std::string_view c) {
        if (!cond.empty()) cond += " || ";
        cond += c;
    };
    if (to & 0x10) add(fmt::format("{} < {}",  aSigned,   bSigned));
    if (to & 0x08) add(fmt::format("{} > {}",  aSigned,   bSigned));
    if (to & 0x04) add(fmt::format("{} == {}", aSigned,   bSigned));
    if (to & 0x02) add(fmt::format("{} < {}",  aUnsigned, bUnsigned));
    if (to & 0x01) add(fmt::format("{} > {}",  aUnsigned, bUnsigned));

    ctx.println("\tif ({}) ppc_trap(ctx, base, 0);", cond);
}

} // namespace rexglue::codegen::builders
