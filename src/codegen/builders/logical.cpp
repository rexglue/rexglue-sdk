/**
 * @file        rexcodegen/builders/logical.cpp
 * @brief       PPC logical instruction code generation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */


#include "helpers.h"

namespace rex::codegen {

//=============================================================================
// AND Operations
//=============================================================================

bool build_and(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = {}.u64 & {}.u64;",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    emitRecordFormCompare(ctx);
    return true;
}

bool build_andc(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = {}.u64 & ~{}.u64;",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    emitRecordFormCompare(ctx);
    return true;
}

bool build_andi(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = {}.u64 & {};",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.insn.operands[2]);
    // ANDI. always sets CR0
    ctx.println("\t{}.compare<int32_t>({}.s32, 0, {});",
        ctx.cr(0), ctx.r(ctx.insn.operands[0]), ctx.xer());
    return true;
}

bool build_andis(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = {}.u64 & {};",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.insn.operands[2] << 16);
    // ANDIS. always sets CR0
    ctx.println("\t{}.compare<int32_t>({}.s32, 0, {});",
        ctx.cr(0), ctx.r(ctx.insn.operands[0]), ctx.xer());
    return true;
}

//=============================================================================
// OR Operations
//=============================================================================

bool build_nand(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = ~({}.u64 & {}.u64);",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    return true;
}

bool build_nor(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = ~({}.u64 | {}.u64);",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    return true;
}

bool build_not(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = ~{}.u64;",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]));
    emitRecordFormCompare(ctx);
    return true;
}

bool build_or(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = {}.u64 | {}.u64;",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    emitRecordFormCompare(ctx);

    // Propagates MMIO base flag if either source register is marked MMIO 
    // covers mr rD,rS which assembles as or rD,rS,rS
    if (ctx.locals.is_mmio_base(ctx.insn.operands[1]) || ctx.locals.is_mmio_base(ctx.insn.operands[2]))
        ctx.locals.set_mmio_base(ctx.insn.operands[0]);
    else
        ctx.locals.clear_mmio_base(ctx.insn.operands[0]);

    return true;
}

bool build_orc(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = {}.u64 | ~{}.u64;",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    return true;
}

bool build_ori(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = {}.u64 | {};",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.insn.operands[2]);

    // ori only sets low bits - propagate MMIO base from source
    if (ctx.locals.is_mmio_base(ctx.insn.operands[1]))
        ctx.locals.set_mmio_base(ctx.insn.operands[0]);
    else
        ctx.locals.clear_mmio_base(ctx.insn.operands[0]);

    return true;
}

bool build_oris(BuilderContext& ctx)
{
    uint32_t imm = static_cast<uint32_t>(ctx.insn.operands[2]);
    size_t dest_reg = ctx.insn.operands[0];

    ctx.println("\t{}.u64 = {}.u64 | {};",
        ctx.r(dest_reg),
        ctx.r(ctx.insn.operands[1]),
        imm << 16);

    if (isMMIOUpperBits(imm)) {
        ctx.locals.set_mmio_base(dest_reg);
    }
    // NOTE(tomc): don't clear flag here - oris may preserve MMIO base from source

    return true;
}

//=============================================================================
// XOR Operations
//=============================================================================

bool build_xor(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = {}.u64 ^ {}.u64;",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    emitRecordFormCompare(ctx);
    return true;
}

bool build_xori(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = {}.u64 ^ {};",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.insn.operands[2]);
    return true;
}

bool build_xoris(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = {}.u64 ^ {};",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.insn.operands[2] << 16);
    return true;
}

bool build_eqv(BuilderContext& ctx)
{
    // eqv: rA = ~(rS ^ rB) (XNOR - equivalent)
    ctx.println("\t{}.u64 = ~({}.u64 ^ {}.u64);",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    emitRecordFormCompare(ctx);
    return true;
}

//=============================================================================
// Count Leading Zeros
//=============================================================================

bool build_cntlzd(BuilderContext& ctx)
{
    ctx.println("\t{0}.u64 = {1}.u64 == 0 ? 64 : __builtin_clzll({1}.u64);",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]));
    return true;
}

bool build_cntlzw(BuilderContext& ctx)
{
    ctx.println("\t{0}.u64 = {1}.u32 == 0 ? 32 : __builtin_clz({1}.u32);",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]));
    return true;
}

//=============================================================================
// Sign Extension
//=============================================================================

bool build_extsb(BuilderContext& ctx)
{
    ctx.println("\t{}.s64 = {}.s8;",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]));
    emitRecordFormCompare(ctx);
    return true;
}

bool build_extsh(BuilderContext& ctx)
{
    ctx.println("\t{}.s64 = {}.s16;",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]));
    emitRecordFormCompare(ctx);
    return true;
}

bool build_extsw(BuilderContext& ctx)
{
    ctx.println("\t{}.s64 = {}.s32;",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]));
    emitRecordFormCompare(ctx);
    return true;
}

//=============================================================================
// Clear Left Word Immediate
//=============================================================================

bool build_clrlwi(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = {}.u32 & 0x{:X};",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        (1ull << (32 - ctx.insn.operands[2])) - 1);
    emitRecordFormCompare(ctx);
    return true;
}

//=============================================================================
// Rotate Left Double Word
//=============================================================================

bool build_rldicl(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = __builtin_rotateleft64({}.u64, {}) & 0x{:X};",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.insn.operands[2],
        compute_mask(ctx.insn.operands[3], 63));
    return true;
}

bool build_rldicr(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = __builtin_rotateleft64({}.u64, {}) & 0x{:X};",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.insn.operands[2],
        compute_mask(0, ctx.insn.operands[3]));
    return true;
}

bool build_rldimi(BuilderContext& ctx)
{
    const uint64_t mask = compute_mask(ctx.insn.operands[3], ~ctx.insn.operands[2]);
    ctx.println("\t{}.u64 = (__builtin_rotateleft64({}.u64, {}) & 0x{:X}) | ({}.u64 & 0x{:X});",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.insn.operands[2],
        mask,
        ctx.r(ctx.insn.operands[0]),
        ~mask);
    return true;
}

bool build_rotldi(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = __builtin_rotateleft64({}.u64, {});",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.insn.operands[2]);
    return true;
}

//=============================================================================
// Rotate Left Word
//=============================================================================

bool build_rlwimi(BuilderContext& ctx)
{
    const uint64_t mask = compute_mask(ctx.insn.operands[3] + 32, ctx.insn.operands[4] + 32);
    ctx.println("\t{}.u64 = (__builtin_rotateleft32({}.u32, {}) & 0x{:X}) | ({}.u64 & 0x{:X});",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.insn.operands[2],
        mask,
        ctx.r(ctx.insn.operands[0]),
        ~mask);
    return true;
}

bool build_rlwinm(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = __builtin_rotateleft64({}.u32 | ({}.u64 << 32), {}) & 0x{:X};",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[1]),
        ctx.insn.operands[2],
        compute_mask(ctx.insn.operands[3] + 32, ctx.insn.operands[4] + 32));
    emitRecordFormCompare(ctx);
    return true;
}

bool build_rlwnm(BuilderContext& ctx)
{
    // Like rlwinm but shift amount comes from register, not immediate
    ctx.println("\t{}.u64 = __builtin_rotateleft64({}.u32 | ({}.u64 << 32), {}.u8 & 0x1F) & 0x{:X};",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]),  // Register, not immediate
        compute_mask(ctx.insn.operands[3] + 32, ctx.insn.operands[4] + 32));
    emitRecordFormCompare(ctx);
    return true;
}

bool build_rotlw(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = __builtin_rotateleft32({}.u32, {}.u8 & 0x1F);",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    return true;
}

bool build_rotlwi(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = __builtin_rotateleft32({}.u32, {});",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.insn.operands[2]);
    emitRecordFormCompare(ctx);
    return true;
}

//=============================================================================
// Shift Left
//=============================================================================

bool build_sld(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = {}.u8 & 0x40 ? 0 : ({}.u64 << ({}.u8 & 0x7F));",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[2]),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    return true;
}

bool build_slw(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = {}.u8 & 0x20 ? 0 : ({}.u32 << ({}.u8 & 0x3F));",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[2]),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    emitRecordFormCompare(ctx);
    return true;
}

//=============================================================================
// Shift Right Algebraic (signed)
//=============================================================================

bool build_srad(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = {}.u64 & 0x7F;", ctx.temp(), ctx.r(ctx.insn.operands[2]));
    ctx.println("\tif ({}.u64 > 0x3F) {}.u64 = 0x3F;", ctx.temp(), ctx.temp());
    ctx.println("\t{}.ca = ({}.s64 < 0) & ((({}.s64 >> {}.u64) << {}.u64) != {}.s64);",
        ctx.xer(),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[1]),
        ctx.temp(),
        ctx.temp(),
        ctx.r(ctx.insn.operands[1]));
    ctx.println("\t{}.s64 = {}.s64 >> {}.u64;",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.temp());
    return true;
}

bool build_sradi(BuilderContext& ctx)
{
    if (ctx.insn.operands[2] != 0)
    {
        ctx.println("\t{}.ca = ({}.s64 < 0) & (({}.u64 & 0x{:X}) != 0);",
            ctx.xer(),
            ctx.r(ctx.insn.operands[1]),
            ctx.r(ctx.insn.operands[1]),
            compute_mask(64 - ctx.insn.operands[2], 63));
        ctx.println("\t{}.s64 = {}.s64 >> {};",
            ctx.r(ctx.insn.operands[0]),
            ctx.r(ctx.insn.operands[1]),
            ctx.insn.operands[2]);
    }
    else
    {
        ctx.println("\t{}.ca = 0;", ctx.xer());
        ctx.println("\t{}.s64 = {}.s64;",
            ctx.r(ctx.insn.operands[0]),
            ctx.r(ctx.insn.operands[1]));
    }
    return true;
}

bool build_sraw(BuilderContext& ctx)
{
    ctx.println("\t{}.u32 = {}.u32 & 0x3F;", ctx.temp(), ctx.r(ctx.insn.operands[2]));
    ctx.println("\tif ({}.u32 > 0x1F) {}.u32 = 0x1F;", ctx.temp(), ctx.temp());
    ctx.println("\t{}.ca = ({}.s32 < 0) & ((({}.s32 >> {}.u32) << {}.u32) != {}.s32);",
        ctx.xer(),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[1]),
        ctx.temp(),
        ctx.temp(),
        ctx.r(ctx.insn.operands[1]));
    ctx.println("\t{}.s64 = {}.s32 >> {}.u32;",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        ctx.temp());
    emitRecordFormCompare(ctx);
    return true;
}

bool build_srawi(BuilderContext& ctx)
{
    if (ctx.insn.operands[2] != 0)
    {
        ctx.println("\t{}.ca = ({}.s32 < 0) & (({}.u32 & 0x{:X}) != 0);",
            ctx.xer(),
            ctx.r(ctx.insn.operands[1]),
            ctx.r(ctx.insn.operands[1]),
            compute_mask(64 - ctx.insn.operands[2], 63));
        ctx.println("\t{}.s64 = {}.s32 >> {};",
            ctx.r(ctx.insn.operands[0]),
            ctx.r(ctx.insn.operands[1]),
            ctx.insn.operands[2]);
    }
    else
    {
        ctx.println("\t{}.ca = 0;", ctx.xer());
        ctx.println("\t{}.s64 = {}.s32;",
            ctx.r(ctx.insn.operands[0]),
            ctx.r(ctx.insn.operands[1]));
    }
    emitRecordFormCompare(ctx);
    return true;
}

//=============================================================================
// Shift Right (unsigned)
//=============================================================================

bool build_srd(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = {}.u8 & 0x40 ? 0 : ({}.u64 >> ({}.u8 & 0x7F));",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[2]),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    return true;
}

bool build_srw(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = {}.u8 & 0x20 ? 0 : ({}.u32 >> ({}.u8 & 0x3F));",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[2]),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    emitRecordFormCompare(ctx);
    return true;
}

} // namespace rexglue::codegen::builders
