/**
 * @file        rexcodegen/builders/memory.cpp
 * @brief       PPC memory instruction code generation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "../builder_context.h"
#include "helpers.h"

namespace rex::codegen {

//=============================================================================
// Load Immediate (not really memory operations, but L* category)
//=============================================================================

bool build_li(BuilderContext& ctx)
{
    ctx.println("\t{}.s64 = {};",
        ctx.r(ctx.insn.operands[0]),
        static_cast<int32_t>(ctx.insn.operands[1]));
    return true;
}

bool build_lis(BuilderContext& ctx)
{
    uint32_t imm = static_cast<uint32_t>(ctx.insn.operands[1]);
    size_t dest_reg = ctx.insn.operands[0];

    ctx.println("\t{}.s64 = {};",
        ctx.r(dest_reg),
        static_cast<int32_t>(imm << 16));

    if (isMMIOUpperBits(imm)) {
        ctx.locals.set_mmio_base(dest_reg);
    } else {
        ctx.locals.clear_mmio_base(dest_reg);
    }

    return true;
}

//=============================================================================
// Byte Loads
//=============================================================================

bool build_lbz(BuilderContext& ctx)
{
    ctx.emit_load_d_form("PPC_LOAD_U8", "u64");
    return true;
}

bool build_lbzu(BuilderContext& ctx)
{
    emitLoadWithUpdate(ctx, "PPC_LOAD_U8");
    return true;
}

bool build_lbzx(BuilderContext& ctx)
{
    ctx.emit_load_x_form("PPC_LOAD_U8", "u64");
    return true;
}

bool build_lbzux(BuilderContext& ctx)
{
    // X-form load with update: EA = rA + rB, then rD = MEM[EA], rA = EA
    // based on lhzux
    ctx.println("\t{} = {}.u32 + {}.u32;",
        ctx.ea(),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    ctx.println("\t{}.u64 = PPC_LOAD_U8({});",
        ctx.r(ctx.insn.operands[0]), ctx.ea());
    ctx.println("\t{}.u32 = {};",
        ctx.r(ctx.insn.operands[1]), ctx.ea());
    return true;
}

//=============================================================================
// Halfword Loads
//=============================================================================

bool build_lha(BuilderContext& ctx)
{
    emitSignExtendLoadDForm(ctx, "int16_t", "PPC_LOAD_U16");
    return true;
}

bool build_lhax(BuilderContext& ctx)
{
    emitSignExtendLoadXForm(ctx, "int16_t", "PPC_LOAD_U16");
    return true;
}

bool build_lhz(BuilderContext& ctx)
{
    ctx.emit_load_d_form("PPC_LOAD_U16", "u64");
    return true;
}

bool build_lhzx(BuilderContext& ctx)
{
    ctx.emit_load_x_form("PPC_LOAD_U16", "u64");
    return true;
}

bool build_lhzu(BuilderContext& ctx)
{
    emitLoadWithUpdate(ctx, "PPC_LOAD_U16");
    return true;
}

bool build_lhzux(BuilderContext& ctx)
{
    // X-form load with update: EA = rA + rB, then rD = MEM[EA], rA = EA
    ctx.println("\t{} = {}.u32 + {}.u32;",
        ctx.ea(),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    ctx.println("\t{}.u64 = PPC_LOAD_U16({});",
        ctx.r(ctx.insn.operands[0]), ctx.ea());
    ctx.println("\t{}.u32 = {};",
        ctx.r(ctx.insn.operands[1]), ctx.ea());
    return true;
}

bool build_lhau(BuilderContext& ctx)
{
    // Load Halfword Algebraic with Update: sign-extend then update rA
    ctx.println("\t{} = {} + {}.u32;",
        ctx.ea(),
        static_cast<int32_t>(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    ctx.println("\t{}.s64 = int16_t(PPC_LOAD_U16({}));",
        ctx.r(ctx.insn.operands[0]), ctx.ea());
    ctx.println("\t{}.u32 = {};",
        ctx.r(ctx.insn.operands[2]), ctx.ea());
    return true;
}

bool build_lhbrx(BuilderContext& ctx)
{
    // Load Halfword Byte-Reverse Indexed
    ctx.print("\t{}.u64 = __builtin_bswap16({}(",
        ctx.r(ctx.insn.operands[0]),
        ctx.mmio_check_x_form() ? "PPC_MM_LOAD_U16" : "PPC_LOAD_U16");
    if (ctx.insn.operands[1] != 0)
        ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[1]));
    ctx.println("{}.u32));", ctx.r(ctx.insn.operands[2]));
    return true;
}

//=============================================================================
// Word Loads
//=============================================================================

bool build_lwa(BuilderContext& ctx)
{
    emitSignExtendLoadDForm(ctx, "int32_t", "PPC_LOAD_U32");
    return true;
}

bool build_lwax(BuilderContext& ctx)
{
    emitSignExtendLoadXForm(ctx, "int32_t", "PPC_LOAD_U32");
    return true;
}

bool build_lwz(BuilderContext& ctx)
{
    ctx.emit_load_d_form("PPC_LOAD_U32", "u64");
    return true;
}

bool build_lwzu(BuilderContext& ctx)
{
    emitLoadWithUpdate(ctx, "PPC_LOAD_U32");
    return true;
}

bool build_lwzx(BuilderContext& ctx)
{
    ctx.emit_load_x_form("PPC_LOAD_U32", "u64");
    return true;
}

bool build_lwzux(BuilderContext& ctx)
{
    // X-form load with update: EA = rA + rB, then rD = MEM[EA], rA = EA
    ctx.println("\t{} = {}.u32 + {}.u32;",
        ctx.ea(),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    ctx.println("\t{}.u64 = PPC_LOAD_U32({});",
        ctx.r(ctx.insn.operands[0]), ctx.ea());
    ctx.println("\t{}.u32 = {};",
        ctx.r(ctx.insn.operands[1]), ctx.ea());
    return true;
}

bool build_lwbrx(BuilderContext& ctx)
{
    ctx.print("\t{}.u64 = __builtin_bswap32({}(",
        ctx.r(ctx.insn.operands[0]),
        ctx.mmio_check_x_form() ? "PPC_MM_LOAD_U32" : "PPC_LOAD_U32");
    if (ctx.insn.operands[1] != 0)
        ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[1]));
    ctx.println("{}.u32));", ctx.r(ctx.insn.operands[2]));
    return true;
}

//=============================================================================
// Doubleword Loads
//=============================================================================

bool build_ld(BuilderContext& ctx)
{
    ctx.emit_load_d_form("PPC_LOAD_U64", "u64");
    return true;
}

bool build_ldu(BuilderContext& ctx)
{
    emitLoadWithUpdate(ctx, "PPC_LOAD_U64");
    return true;
}

bool build_ldx(BuilderContext& ctx)
{
    ctx.emit_load_x_form("PPC_LOAD_U64", "u64");
    return true;
}

bool build_ldux(BuilderContext& ctx)
{
    // X-form load with update: EA = rA + rB, then rD = MEM[EA], rA = EA
    ctx.println("\t{} = {}.u32 + {}.u32;",
        ctx.ea(),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    ctx.println("\t{}.u64 = PPC_LOAD_U64({});",
        ctx.r(ctx.insn.operands[0]), ctx.ea());
    ctx.println("\t{}.u32 = {};",
        ctx.r(ctx.insn.operands[1]), ctx.ea());
    return true;
}

//=============================================================================
// Atomic Load and Reserve
//=============================================================================

bool build_lwarx(BuilderContext& ctx)
{
    // Compute effective address first, then apply physical offset
    ctx.print("\t{} = ", ctx.ea());
    if (ctx.insn.operands[1] != 0)
        ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[1]));
    ctx.println("{}.u32;", ctx.r(ctx.insn.operands[2]));
    ctx.println("\t{}.u32 = *(uint32_t*)PPC_RAW_ADDR({});", ctx.reserved(), ctx.ea());
    ctx.println("\t{}.u64 = __builtin_bswap32({}.u32);",
        ctx.r(ctx.insn.operands[0]), ctx.reserved());
    return true;
}

bool build_ldarx(BuilderContext& ctx)
{
    // Compute effective address first, then apply physical offset
    ctx.print("\t{} = ", ctx.ea());
    if (ctx.insn.operands[1] != 0)
        ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[1]));
    ctx.println("{}.u32;", ctx.r(ctx.insn.operands[2]));
    ctx.println("\t{}.u64 = *(uint64_t*)PPC_RAW_ADDR({});", ctx.reserved(), ctx.ea());
    ctx.println("\t{}.u64 = __builtin_bswap64({}.u64);",
        ctx.r(ctx.insn.operands[0]), ctx.reserved());
    return true;
}

//=============================================================================
// Floating Point Loads
//=============================================================================

bool build_lfd(BuilderContext& ctx)
{
    ctx.emit_set_flush_mode(false);
    ctx.print("\t{}.u64 = PPC_LOAD_U64(", ctx.f(ctx.insn.operands[0]));
    if (ctx.insn.operands[2] != 0)
        ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[2]));
    ctx.println("{});", static_cast<int32_t>(ctx.insn.operands[1]));
    return true;
}

bool build_lfdx(BuilderContext& ctx)
{
    ctx.emit_set_flush_mode(false);
    ctx.print("\t{}.u64 = PPC_LOAD_U64(", ctx.f(ctx.insn.operands[0]));
    if (ctx.insn.operands[1] != 0)
        ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[1]));
    ctx.println("{}.u32);", ctx.r(ctx.insn.operands[2]));
    return true;
}

bool build_lfs(BuilderContext& ctx)
{
    ctx.emit_set_flush_mode(false);
    ctx.print("\t{}.u32 = PPC_LOAD_U32(", ctx.temp());
    if (ctx.insn.operands[2] != 0)
        ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[2]));
    ctx.println("{});", static_cast<int32_t>(ctx.insn.operands[1]));
    ctx.println("\t{}.f64 = double({}.f32);", ctx.f(ctx.insn.operands[0]), ctx.temp());
    return true;
}

bool build_lfsx(BuilderContext& ctx)
{
    ctx.emit_set_flush_mode(false);
    ctx.print("\t{}.u32 = PPC_LOAD_U32(", ctx.temp());
    if (ctx.insn.operands[1] != 0)
        ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[1]));
    ctx.println("{}.u32);", ctx.r(ctx.insn.operands[2]));
    ctx.println("\t{}.f64 = double({}.f32);", ctx.f(ctx.insn.operands[0]), ctx.temp());
    return true;
}

bool build_lfdu(BuilderContext& ctx)
{
    // Load Floating-point Double with Update
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{} = {} + {}.u32;",
        ctx.ea(),
        static_cast<int32_t>(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    ctx.println("\t{}.u64 = PPC_LOAD_U64({});",
        ctx.f(ctx.insn.operands[0]), ctx.ea());
    ctx.println("\t{}.u32 = {};",
        ctx.r(ctx.insn.operands[2]), ctx.ea());
    return true;
}

bool build_lfdux(BuilderContext& ctx)
{
    // Load Floating-point Double with Update Indexed
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{} = {}.u32 + {}.u32;",
        ctx.ea(),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    ctx.println("\t{}.u64 = PPC_LOAD_U64({});",
        ctx.f(ctx.insn.operands[0]), ctx.ea());
    ctx.println("\t{}.u32 = {};",
        ctx.r(ctx.insn.operands[1]), ctx.ea());
    return true;
}

bool build_lfsu(BuilderContext& ctx)
{
    // Load Floating-point Single with Update (convert to double)
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{} = {} + {}.u32;",
        ctx.ea(),
        static_cast<int32_t>(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    ctx.println("\t{}.u32 = PPC_LOAD_U32({});",
        ctx.temp(), ctx.ea());
    ctx.println("\t{}.f64 = double({}.f32);",
        ctx.f(ctx.insn.operands[0]), ctx.temp());
    ctx.println("\t{}.u32 = {};",
        ctx.r(ctx.insn.operands[2]), ctx.ea());
    return true;
}

bool build_lfsux(BuilderContext& ctx)
{
    // Load Floating-point Single with Update Indexed (convert to double)
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{} = {}.u32 + {}.u32;",
        ctx.ea(),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    ctx.println("\t{}.u32 = PPC_LOAD_U32({});",
        ctx.temp(), ctx.ea());
    ctx.println("\t{}.f64 = double({}.f32);",
        ctx.f(ctx.insn.operands[0]), ctx.temp());
    ctx.println("\t{}.u32 = {};",
        ctx.r(ctx.insn.operands[1]), ctx.ea());
    return true;
}

//=============================================================================
// Byte Stores
//=============================================================================

bool build_stb(BuilderContext& ctx)
{
    ctx.emit_store_d_form("PPC_STORE_U8", "u8", true);
    return true;
}

bool build_stbu(BuilderContext& ctx)
{
    emitStoreWithUpdate(ctx, "PPC_STORE_U8", "u8");
    return true;
}

bool build_stbx(BuilderContext& ctx)
{
    ctx.emit_store_x_form("PPC_STORE_U8", "u8", true);
    return true;
}

bool build_stbux(BuilderContext& ctx)
{
    // X-form store with update: EA = rA + rB, store, then rA = EA
    ctx.println("\t{} = {}.u32 + {}.u32;",
        ctx.ea(),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    ctx.println("\t{}({}, {}.u8);",
        ctx.mmio_check_x_form() ? "PPC_MM_STORE_U8" : "PPC_STORE_U8",
        ctx.ea(),
        ctx.r(ctx.insn.operands[0]));
    ctx.println("\t{}.u32 = {};",
        ctx.r(ctx.insn.operands[1]), ctx.ea());
    return true;
}

//=============================================================================
// Halfword Stores
//=============================================================================

bool build_sth(BuilderContext& ctx)
{
    ctx.emit_store_d_form("PPC_STORE_U16", "u16", true);
    return true;
}

bool build_sthbrx(BuilderContext& ctx)
{
    ctx.print("{}", ctx.mmio_check_x_form() ? "\tPPC_MM_STORE_U16(" : "\tPPC_STORE_U16(");
    if (ctx.insn.operands[1] != 0)
        ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[1]));
    ctx.println("{}.u32, __builtin_bswap16({}.u16));",
        ctx.r(ctx.insn.operands[2]), ctx.r(ctx.insn.operands[0]));
    return true;
}

bool build_sthx(BuilderContext& ctx)
{
    ctx.emit_store_x_form("PPC_STORE_U16", "u16", true);
    return true;
}

bool build_sthu(BuilderContext& ctx)
{
    emitStoreWithUpdate(ctx, "PPC_STORE_U16", "u16");
    return true;
}

bool build_sthux(BuilderContext& ctx)
{
    // X-form store with update: EA = rA + rB, store, then rA = EA
    ctx.println("\t{} = {}.u32 + {}.u32;",
        ctx.ea(),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    ctx.println("\tPPC_STORE_U16({}, {}.u16);",
        ctx.ea(), ctx.r(ctx.insn.operands[0]));
    ctx.println("\t{}.u32 = {};",
        ctx.r(ctx.insn.operands[1]), ctx.ea());
    return true;
}

//=============================================================================
// Word Stores
//=============================================================================

bool build_stw(BuilderContext& ctx)
{
    ctx.emit_store_d_form("PPC_STORE_U32", "u32", true);
    return true;
}

bool build_stwu(BuilderContext& ctx)
{
    emitStoreWithUpdate(ctx, "PPC_STORE_U32", "u32");
    return true;
}

bool build_stwux(BuilderContext& ctx)
{
    ctx.println("\t{} = {}.u32 + {}.u32;",
        ctx.ea(),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    ctx.println("\tPPC_STORE_U32({}, {}.u32);", ctx.ea(), ctx.r(ctx.insn.operands[0]));
    ctx.println("\t{}.u32 = {};", ctx.r(ctx.insn.operands[1]), ctx.ea());
    return true;
}

bool build_stwx(BuilderContext& ctx)
{
    ctx.emit_store_x_form("PPC_STORE_U32", "u32", true);
    return true;
}

bool build_stwbrx(BuilderContext& ctx)
{
    ctx.print("{}", ctx.mmio_check_x_form() ? "\tPPC_MM_STORE_U32(" : "\tPPC_STORE_U32(");
    if (ctx.insn.operands[1] != 0)
        ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[1]));
    ctx.println("{}.u32, __builtin_bswap32({}.u32));",
        ctx.r(ctx.insn.operands[2]), ctx.r(ctx.insn.operands[0]));
    return true;
}

//=============================================================================
// Atomic Store Conditional
//=============================================================================

bool build_stwcx(BuilderContext& ctx)
{
    // Compute effective address first, then apply physical offset
    ctx.print("\t{} = ", ctx.ea());
    if (ctx.insn.operands[1] != 0)
        ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[1]));
    ctx.println("{}.u32;", ctx.r(ctx.insn.operands[2]));
    ctx.println("\t{}.lt = 0;", ctx.cr(0));
    ctx.println("\t{}.gt = 0;", ctx.cr(0));
    ctx.println("\t{}.eq = __sync_bool_compare_and_swap(reinterpret_cast<uint32_t*>(PPC_RAW_ADDR({})), {}.s32, __builtin_bswap32({}.s32));",
        ctx.cr(0), ctx.ea(), ctx.reserved(), ctx.r(ctx.insn.operands[0]));
    ctx.println("\t{}.so = {}.so;", ctx.cr(0), ctx.xer());
    return true;
}

bool build_stdcx(BuilderContext& ctx)
{
    // Compute effective address first, then apply physical offset
    ctx.print("\t{} = ", ctx.ea());
    if (ctx.insn.operands[1] != 0)
        ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[1]));
    ctx.println("{}.u32;", ctx.r(ctx.insn.operands[2]));
    ctx.println("\t{}.lt = 0;", ctx.cr(0));
    ctx.println("\t{}.gt = 0;", ctx.cr(0));
    ctx.println("\t{}.eq = __sync_bool_compare_and_swap(reinterpret_cast<uint64_t*>(PPC_RAW_ADDR({})), {}.s64, __builtin_bswap64({}.s64));",
        ctx.cr(0), ctx.ea(), ctx.reserved(), ctx.r(ctx.insn.operands[0]));
    ctx.println("\t{}.so = {}.so;", ctx.cr(0), ctx.xer());
    return true;
}

//=============================================================================
// Doubleword Stores
//=============================================================================

bool build_std(BuilderContext& ctx)
{
    ctx.emit_store_d_form("PPC_STORE_U64", "u64", true);
    return true;
}

bool build_stdu(BuilderContext& ctx)
{
    ctx.println("\t{} = {} + {}.u32;",
        ctx.ea(),
        static_cast<int32_t>(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    ctx.println("\tPPC_STORE_U64({}, {}.u64);", ctx.ea(), ctx.r(ctx.insn.operands[0]));
    ctx.println("\t{}.u32 = {};", ctx.r(ctx.insn.operands[2]), ctx.ea());
    return true;
}

bool build_stdx(BuilderContext& ctx)
{
    ctx.emit_store_x_form("PPC_STORE_U64", "u64", true);
    return true;
}

bool build_stdux(BuilderContext& ctx)
{
    // X-form store with update: EA = rA + rB, store, then rA = EA
    ctx.println("\t{} = {}.u32 + {}.u32;",
        ctx.ea(),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    ctx.println("\t{}({}, {}.u64);",
        ctx.mmio_check_x_form() ? "PPC_MM_STORE_U64" : "PPC_STORE_U64",
        ctx.ea(),
        ctx.r(ctx.insn.operands[0]));
    ctx.println("\t{}.u32 = {};",
        ctx.r(ctx.insn.operands[1]), ctx.ea());
    return true;
}

//=============================================================================
// Floating Point Stores
//=============================================================================

bool build_stfd(BuilderContext& ctx)
{
    ctx.emit_set_flush_mode(false);
    ctx.print("{}", ctx.mmio_check_d_form() ? "\tPPC_MM_STORE_U64(" : "\tPPC_STORE_U64(");
    if (ctx.insn.operands[2] != 0)
        ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[2]));
    ctx.println("{}, {}.u64);",
        static_cast<int32_t>(ctx.insn.operands[1]),
        ctx.f(ctx.insn.operands[0]));
    return true;
}

bool build_stfdx(BuilderContext& ctx)
{
    ctx.emit_set_flush_mode(false);
    ctx.print("{}", ctx.mmio_check_x_form() ? "\tPPC_MM_STORE_U64(" : "\tPPC_STORE_U64(");
    if (ctx.insn.operands[1] != 0)
        ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[1]));
    ctx.println("{}.u32, {}.u64);", ctx.r(ctx.insn.operands[2]), ctx.f(ctx.insn.operands[0]));
    return true;
}

bool build_stfiwx(BuilderContext& ctx)
{
    ctx.emit_set_flush_mode(false);
    ctx.print("{}", ctx.mmio_check_x_form() ? "\tPPC_MM_STORE_U32(" : "\tPPC_STORE_U32(");
    if (ctx.insn.operands[1] != 0)
        ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[1]));
    ctx.println("{}.u32, {}.u32);", ctx.r(ctx.insn.operands[2]), ctx.f(ctx.insn.operands[0]));
    return true;
}

bool build_stfs(BuilderContext& ctx)
{
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f32 = float({}.f64);", ctx.temp(), ctx.f(ctx.insn.operands[0]));
    ctx.print("{}", ctx.mmio_check_d_form() ? "\tPPC_MM_STORE_U32(" : "\tPPC_STORE_U32(");
    if (ctx.insn.operands[2] != 0)
        ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[2]));
    ctx.println("{}, {}.u32);",
        static_cast<int32_t>(ctx.insn.operands[1]),
        ctx.temp());
    return true;
}

bool build_stfsx(BuilderContext& ctx)
{
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f32 = float({}.f64);", ctx.temp(), ctx.f(ctx.insn.operands[0]));
    ctx.print("{}", ctx.mmio_check_x_form() ? "\tPPC_MM_STORE_U32(" : "\tPPC_STORE_U32(");
    if (ctx.insn.operands[1] != 0)
        ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[1]));
    ctx.println("{}.u32, {}.u32);", ctx.r(ctx.insn.operands[2]), ctx.temp());
    return true;
}

bool build_stfdu(BuilderContext& ctx)
{
    // Store Floating-point Double with Update
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{} = {} + {}.u32;",
        ctx.ea(),
        static_cast<int32_t>(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    ctx.println("\tPPC_STORE_U64({}, {}.u64);",
        ctx.ea(), ctx.f(ctx.insn.operands[0]));
    ctx.println("\t{}.u32 = {};",
        ctx.r(ctx.insn.operands[2]), ctx.ea());
    return true;
}

bool build_stfsu(BuilderContext& ctx)
{
    // Store Floating-point Single with Update (convert double to float first)
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{} = {} + {}.u32;",
        ctx.ea(),
        static_cast<int32_t>(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    ctx.println("\t{}.f32 = float({}.f64);",
        ctx.temp(), ctx.f(ctx.insn.operands[0]));
    ctx.println("\tPPC_STORE_U32({}, {}.u32);",
        ctx.ea(), ctx.temp());
    ctx.println("\t{}.u32 = {};",
        ctx.r(ctx.insn.operands[2]), ctx.ea());
    return true;
}

bool build_stfsux(BuilderContext& ctx)
{
    // Store Floating-point Single with Update Indexed
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f32 = float({}.f64);",
        ctx.temp(), ctx.f(ctx.insn.operands[0]));
    ctx.println("\t{} = {}.u32 + {}.u32;",
        ctx.ea(),
        ctx.r(ctx.insn.operands[1]),
        ctx.r(ctx.insn.operands[2]));
    ctx.println("\t{}({}, {}.u32);",
        ctx.mmio_check_x_form() ? "PPC_MM_STORE_U32" : "PPC_STORE_U32",
        ctx.ea(),
        ctx.temp());
    ctx.println("\t{}.u32 = {};",
        ctx.r(ctx.insn.operands[1]), ctx.ea());
    return true;
}

//=============================================================================
// Vector Loads (will be more comprehensive in vector.cpp)
//=============================================================================

bool build_lvx(BuilderContext& ctx)
{
    // NOTE(tomc): for endian swapping, we reverse the whole vector instead of individual elements.
    // this is accounted for in every instruction (eg. dp3 sums yzw instead of xyz)
    emitVectorEA(ctx, "0xF");
    ctx.println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)PPC_RAW_ADDR({})), simde_mm_load_si128((simde__m128i*)VectorMaskL)));",
        ctx.v(ctx.insn.operands[0]), ctx.ea());
    return true;
}

bool build_lvlx(BuilderContext& ctx)
{
    emitVectorTempEA(ctx);
    ctx.println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)PPC_RAW_ADDR({}.u32 & ~0xF)), simde_mm_load_si128((simde__m128i*)&VectorMaskL[({}.u32 & 0xF) * 16])));",
        ctx.v(ctx.insn.operands[0]), ctx.temp(), ctx.temp());
    return true;
}

bool build_lvrx(BuilderContext& ctx)
{
    emitVectorTempEA(ctx);
    ctx.println("\tsimde_mm_store_si128((simde__m128i*){}.u8, {}.u32 & 0xF ? simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)PPC_RAW_ADDR({}.u32 & ~0xF)), simde_mm_load_si128((simde__m128i*)&VectorMaskR[({}.u32 & 0xF) * 16])) : simde_mm_setzero_si128());",
        ctx.v(ctx.insn.operands[0]), ctx.temp(), ctx.temp(), ctx.temp());
    return true;
}

bool build_lvsl(BuilderContext& ctx)
{
    emitVectorTempEA(ctx);
    ctx.println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_load_si128((simde__m128i*)&VectorShiftTableL[({}.u32 & 0xF) * 16]));",
        ctx.v(ctx.insn.operands[0]), ctx.temp());
    return true;
}

bool build_lvsr(BuilderContext& ctx)
{
    emitVectorTempEA(ctx);
    ctx.println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_load_si128((simde__m128i*)&VectorShiftTableR[({}.u32 & 0xF) * 16]));",
        ctx.v(ctx.insn.operands[0]), ctx.temp());
    return true;
}

//=============================================================================
// Vector Stores
//=============================================================================

bool build_stvehx(BuilderContext& ctx)
{
    // TODO(tomc): vectorize
    // NOTE: accounting for the full vector reversal here
    emitVectorEA(ctx, "0x1");
    ctx.println("\tPPC_STORE_U16(ea, {}.u16[7 - (({} & 0xF) >> 1)]);",
        ctx.v(ctx.insn.operands[0]), ctx.ea());
    return true;
}

bool build_stvewx(BuilderContext& ctx)
{
    // TODO(tomc): vectorize
    // NOTE: accounting for the full vector reversal here
    emitVectorEA(ctx, "0x3");
    ctx.println("\tPPC_STORE_U32(ea, {}.u32[3 - (({} & 0xF) >> 2)]);",
        ctx.v(ctx.insn.operands[0]), ctx.ea());
    return true;
}

bool build_stvlx(BuilderContext& ctx)
{
    // TODO(tomc): vectorize
    // NOTE: accounting for the full vector reversal here
    emitVectorEA(ctx);

    ctx.println("\tfor (size_t i = 0; i < (16 - ({} & 0xF)); i++)", ctx.ea());
    ctx.println("\t\tPPC_STORE_U8({} + i, {}.u8[15 - i]);", ctx.ea(), ctx.v(ctx.insn.operands[0]));
    return true;
}

bool build_stvrx(BuilderContext& ctx)
{
    // TODO(tomc): vectorize
    // NOTE: accounting for the full vector reversal here
    emitVectorEA(ctx);

    ctx.println("\tfor (size_t i = 0; i < ({} & 0xF); i++)", ctx.ea());
    ctx.println("\t\tPPC_STORE_U8({} - i - 1, {}.u8[i]);", ctx.ea(), ctx.v(ctx.insn.operands[0]));
    return true;
}

bool build_stvx(BuilderContext& ctx)
{
    emitVectorEA(ctx, "0xF");
    ctx.println("\tsimde_mm_store_si128((simde__m128i*)PPC_RAW_ADDR({}), simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*){}.u8), simde_mm_load_si128((simde__m128i*)VectorMaskL)));",
        ctx.ea(), ctx.v(ctx.insn.operands[0]));
    return true;
}

} // namespace rexglue::codegen::builders
