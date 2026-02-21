/**
 * @file        rexcodegen/builders/system.cpp
 * @brief       PPC system instruction code generation
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
// No-ops and Sync Operations
//=============================================================================

bool build_nop(BuilderContext& ctx)
{
    // Canonical PPC no-op (ori 0,0,0)
    (void)ctx;
    return true;
}

bool build_attn(BuilderContext& ctx)
{
    // Xenon-specific debug breakpoint, no effect in recompiled code
    (void)ctx;
    return true;
}

bool build_sync(BuilderContext& ctx)
{
    // Memory barrier, x86 has strong ordering so this is a no-op
    (void)ctx;
    return true;
}

bool build_lwsync(BuilderContext& ctx)
{
    // Lightweight memory barrier, x86 has strong ordering so this is a no-op
    (void)ctx;
    return true;
}

bool build_eieio(BuilderContext& ctx)
{
    // Enforce in-order execution of I/O, x86 has strong ordering so this is a no-op
    (void)ctx;
    return true;
}

bool build_db16cyc(BuilderContext& ctx)
{
    // Xenon-specific 16-cycle delay hint, no effect in recompiled code
    (void)ctx;
    return true;
}

bool build_cctpl(BuilderContext& ctx)
{
    // Xenon-specific cache control thread priority low, no effect in recompiled code
    (void)ctx;
    return true;
}

bool build_cctpm(BuilderContext& ctx)
{
    // Xenon-specific cache control thread priority medium, no effect in recompiled code
    (void)ctx;
    return true;
}

//=============================================================================
// Trap Instructions
// PPC trap instructions are assertion/debug checks. The TO field (bits 21-25)
// is a 5-bit mask specifying which conditions trigger: signed lt/gt, eq,
// unsigned lt/gt. We extract TO directly from the instruction word so that
// both generic (tw TO,rA,rB) and simplified (tweq rA,rB) forms work with
// the same builder.
//=============================================================================

bool build_tdi(BuilderContext& ctx)
{
    uint32_t to = (ctx.insn.instruction >> 21) & 0x1F;
    uint32_t ra = (ctx.insn.instruction >> 16) & 0x1F;
    int64_t simm = static_cast<int16_t>(ctx.insn.instruction & 0xFFFF);
    emitTrap(ctx, to,
        fmt::format("{}.s64", ctx.r(ra)), fmt::format("{}.u64", ctx.r(ra)),
        fmt::format("{}ll", simm), fmt::format("{}ull", static_cast<uint64_t>(simm)));
    return true;
}

bool build_twi(BuilderContext& ctx)
{
    uint32_t to = (ctx.insn.instruction >> 21) & 0x1F;
    uint32_t ra = (ctx.insn.instruction >> 16) & 0x1F;
    int32_t simm = static_cast<int16_t>(ctx.insn.instruction & 0xFFFF);

    // twi 31, r0, <imm> is an unconditional trap with service code in the immediate
    if (to == 0x1F && ra == 0) {
        uint16_t trap_type = static_cast<uint16_t>(simm);
        ctx.println("\tppc_trap(ctx, base, {});", trap_type);
        return true;
    }

    emitTrap(ctx, to,
        fmt::format("{}.s32", ctx.r(ra)), fmt::format("{}.u32", ctx.r(ra)),
        fmt::format("{}", simm), fmt::format("{}u", static_cast<uint32_t>(simm)));
    return true;
}

bool build_td(BuilderContext& ctx)
{
    uint32_t to = (ctx.insn.instruction >> 21) & 0x1F;
    uint32_t ra = (ctx.insn.instruction >> 16) & 0x1F;
    uint32_t rb = (ctx.insn.instruction >> 11) & 0x1F;
    emitTrap(ctx, to,
        fmt::format("{}.s64", ctx.r(ra)), fmt::format("{}.u64", ctx.r(ra)),
        fmt::format("{}.s64", ctx.r(rb)), fmt::format("{}.u64", ctx.r(rb)));
    return true;
}

bool build_tw(BuilderContext& ctx)
{
    uint32_t to = (ctx.insn.instruction >> 21) & 0x1F;
    uint32_t ra = (ctx.insn.instruction >> 16) & 0x1F;
    uint32_t rb = (ctx.insn.instruction >> 11) & 0x1F;
    emitTrap(ctx, to,
        fmt::format("{}.s32", ctx.r(ra)), fmt::format("{}.u32", ctx.r(ra)),
        fmt::format("{}.s32", ctx.r(rb)), fmt::format("{}.u32", ctx.r(rb)));
    return true;
}

//=============================================================================
// Cache Operations
//=============================================================================

bool build_dcbf(BuilderContext& ctx)
{
    // Hint instruction, access violation callback handlers take care of this on write
    (void)ctx;
    return true;
}

bool build_dcbt(BuilderContext& ctx)
{
    // Hint instruction, prefetch has no semantic effect
    (void)ctx;
    return true;
}

bool build_dcbtst(BuilderContext& ctx)
{
    // Hint instruction, prefetch-for-store has no semantic effect
    (void)ctx;
    return true;
}

bool build_dcbz(BuilderContext& ctx)
{
    // Compute EA, align to 32-byte cache line, apply physical offset
    ctx.print("\t{} = (", ctx.ea());
    if (ctx.insn.operands[0] != 0)
        ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[0]));
    ctx.println("{}.u32) & ~31;", ctx.r(ctx.insn.operands[1]));
    ctx.println("\tmemset((void*)PPC_RAW_ADDR({}), 0, 32);", ctx.ea());
    return true;
}

bool build_dcbzl(BuilderContext& ctx)
{
    // Compute EA, align to 128-byte cache line, apply physical offset
    ctx.print("\t{} = (", ctx.ea());
    if (ctx.insn.operands[0] != 0)
        ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[0]));
    ctx.println("{}.u32) & ~127;", ctx.r(ctx.insn.operands[1]));
    ctx.println("\tmemset((void*)PPC_RAW_ADDR({}), 0, 128);", ctx.ea());
    return true;
}

bool build_dcbst(BuilderContext& ctx)
{
    // Hint instruction, access violation callback handlers take care of this on write
    (void)ctx;
    return true;
}

//=============================================================================
// Move Register
//=============================================================================

bool build_mr(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = {}.u64;",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]));
    emitRecordFormCompare(ctx);

    // Propagates MMIO base flag from source to destination register
    if (ctx.locals.is_mmio_base(ctx.insn.operands[1]))
        ctx.locals.set_mmio_base(ctx.insn.operands[0]);
    else
        ctx.locals.clear_mmio_base(ctx.insn.operands[0]);

    return true;
}

//=============================================================================
// Move From Special Registers
//=============================================================================

bool build_mfcr(BuilderContext& ctx)
{
    for (size_t i = 0; i < 32; i++)
    {
        constexpr std::string_view fields[] = { "lt", "gt", "eq", "so" };
        ctx.println("\t{}.u64 {}= {}.{} ? 0x{:X} : 0;",
            ctx.r(ctx.insn.operands[0]),
            i == 0 ? "" : "|",
            ctx.cr(i / 4),
            fields[i % 4],
            1u << (31 - i));
    }
    return true;
}

bool build_mfocrf(BuilderContext& ctx)
{
    // FXM is a one-hot mask: bit 7 = CR0, bit 6 = CR1, ..., bit 0 = CR7
    uint32_t fxm = ctx.insn.operands[1];
    uint32_t crField = 0;
    for (uint32_t i = 0; i < 8; i++) {
        if (fxm & (0x80u >> i)) {
            crField = i;
            break;
        }
    }
    uint32_t baseShift = 28 - 4 * crField;
    ctx.println("\t{}.u64 = ({}.lt << {}) | ({}.gt << {}) | ({}.eq << {}) | ({}.so << {});",
        ctx.r(ctx.insn.operands[0]),
        ctx.cr(crField), baseShift + 3,
        ctx.cr(crField), baseShift + 2,
        ctx.cr(crField), baseShift + 1,
        ctx.cr(crField), baseShift);
    return true;
}

bool build_mflr(BuilderContext& ctx)
{
    if (!ctx.config().skipLr)
        ctx.println("\t{}.u64 = ctx.lr;", ctx.r(ctx.insn.operands[0]));
    return true;
}

bool build_mfmsr(BuilderContext& ctx)
{
    if (!ctx.config().skipMsr) {
        // Memory barrier for MSR read
        ctx.println("\tstd::atomic_thread_fence(std::memory_order_seq_cst);");
        // Check global lock and return appropriate value
        // Returns 0x8000 if unlocked (interrupts enabled), 0 if locked
        ctx.println("\t{}.u64 = PPC_CHECK_GLOBAL_LOCK();",
            ctx.r(ctx.insn.operands[0]));
    }
    return true;
}

bool build_mffs(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = ctx.fpscr.loadFromHost();", ctx.f(ctx.insn.operands[0]));
    return true;
}

bool build_mftb(BuilderContext& ctx)
{
    // Xbox 360 timebase runs at 50 MHz (guest tick frequency)
    // Using PPC_QUERY_TIMEBASE() macro provides properly scaled timing from the runtime
    ctx.println("\t{}.u64 = PPC_QUERY_TIMEBASE();", ctx.r(ctx.insn.operands[0]));
    return true;
}

//=============================================================================
// Move To Special Registers
//=============================================================================

bool build_mtcr(BuilderContext& ctx)
{
    for (size_t i = 0; i < 32; i++)
    {
        constexpr std::string_view fields[] = { "lt", "gt", "eq", "so" };
        ctx.println("\t{}.{} = ({}.u32 & 0x{:X}) != 0;",
            ctx.cr(i / 4),
            fields[i % 4],
            ctx.r(ctx.insn.operands[0]),
            1u << (31 - i));
    }
    return true;
}

bool build_mtctr(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = {}.u64;", ctx.ctr(), ctx.r(ctx.insn.operands[0]));
    return true;
}

bool build_mtlr(BuilderContext& ctx)
{
    if (!ctx.config().skipLr)
        ctx.println("\tctx.lr = {}.u64;", ctx.r(ctx.insn.operands[0]));
    return true;
}

bool build_mtmsrd(BuilderContext& ctx)
{
    if (!ctx.config().skipMsr) {
        // Memory barrier for MSR write
        ctx.println("\tstd::atomic_thread_fence(std::memory_order_seq_cst);");
        // Update MSR bits
        ctx.println("\tctx.msr = ({}.u32 & 0x8020) | (ctx.msr & ~0x8020);",
            ctx.r(ctx.insn.operands[0]));
        // Global lock mechanism:
        // R13 = enter lock (disable interrupts)
        // Other = leave lock (enable interrupts)
        uint32_t src_reg = ctx.insn.operands[0];
        if (src_reg == 13) {
            ctx.println("\tPPC_ENTER_GLOBAL_LOCK();");
        } else {
            ctx.println("\tPPC_LEAVE_GLOBAL_LOCK();");
        }
    }
    return true;
}

bool build_mtfsf(BuilderContext& ctx)
{
    ctx.println("\tctx.fpscr.storeFromGuest({}.u32);", ctx.f(ctx.insn.operands[1]));
    return true;
}

bool build_mtxer(BuilderContext& ctx)
{
    ctx.println("\t{}.so = ({}.u64 & 0x80000000) != 0;",
        ctx.xer(), ctx.r(ctx.insn.operands[0]));
    ctx.println("\t{}.ov = ({}.u64 & 0x40000000) != 0;",
        ctx.xer(), ctx.r(ctx.insn.operands[0]));
    ctx.println("\t{}.ca = ({}.u64 & 0x20000000) != 0;",
        ctx.xer(), ctx.r(ctx.insn.operands[0]));
    return true;
}

//=============================================================================
// Clear Left Double Word Immediate
//=============================================================================

bool build_clrldi(BuilderContext& ctx)
{
    ctx.println("\t{}.u64 = {}.u64 & 0x{:X};",
        ctx.r(ctx.insn.operands[0]),
        ctx.r(ctx.insn.operands[1]),
        (1ull << (64 - ctx.insn.operands[2])) - 1);
    return true;
}

} // namespace rexglue::codegen::builders
