/**
 * @file        rexcodegen/builders/control_flow.cpp
 * @brief       PPC control flow instruction code generation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "../builder_context.h"
#include <rex/logging.h>
#include <fmt/format.h>
#include "helpers.h"

namespace rex::codegen {

//=============================================================================
// Unconditional Branch
//=============================================================================

bool build_b(BuilderContext& ctx)
{
    uint32_t target = ctx.insn.operands[0];

    // Use graph to classify the target - handles thunks that branch to nearby functions
    // false = branch instruction (not a call), so own-base means loop back
    auto kind = ctx.graph().classifyTarget(target, ctx.base, false);

    switch (kind) {
    case TargetKind::InternalLabel:
        // Target is within this function and not another function's entry point
        ctx.println("\tgoto loc_{:X};", target);
        break;

    case TargetKind::Function:
    case TargetKind::Import:
        // Tail call to another function or import
        ctx.emit_function_call(target);
        ctx.println("\treturn;");
        break;

    case TargetKind::Unknown:
        // Unknown target - fall back to range check
        if (target >= ctx.fn.base() && target < ctx.fn.end()) {
            ctx.println("\tgoto loc_{:X};", target);
        } else {
            REXCODEGEN_WARN("Unresolved b target 0x{:08X} from 0x{:08X}", target, ctx.base);
            ctx.emit_function_call(target);
            ctx.println("\treturn;");
        }
        break;
    }
    return true;
}

bool build_bl(BuilderContext& ctx)
{
    uint32_t target = ctx.insn.operands[0];

    // Always set LR (unless skipLr)
    if (!ctx.config().skipLr)
        ctx.println("\tctx.lr = 0x{:X};", ctx.base + 4);

    // Use graph to classify the target
    // true = call instruction, so own-base means recursive call (not loop back)
    auto kind = ctx.graph().classifyTarget(target, ctx.base, true);

    switch (kind) {
    case TargetKind::InternalLabel:
        // PIC code pattern - bl to get PC into LR, treat as local jump
        // LR is already set above, now jump to the target
        ctx.println("\tgoto loc_{:X};", target);
        break;

    case TargetKind::Function:
    case TargetKind::Import:
        ctx.emit_function_call(target);
        ctx.csrState = CSRState::Unknown;  // Call could change CSR state
        break;

    case TargetKind::Unknown:
        REXCODEGEN_ERROR("Unresolved bl target 0x{:08X} from 0x{:08X}", target, ctx.base);
        ctx.println("\t// ERROR: unresolved bl target 0x{:08X}", target);
        ctx.println("\tREX_FATAL(\"Unresolved call from 0x{:08X} to 0x{:08X}\");", ctx.base, target);
        break;
    }
    return true;
}

bool build_blr(BuilderContext& ctx)
{
    ctx.println("\treturn;");
    return true;
}

bool build_blrl(BuilderContext& ctx)
{
    ctx.println("__builtin_debugtrap();");
    return true;
}

//=============================================================================
// Count Register Branch
//=============================================================================

bool build_bctr(BuilderContext& ctx)
{
    // Check config first (manual override), then auto-detected
    const JumpTable* jt = nullptr;

    if (ctx.switchTable != ctx.config().switchTables.end()) {
        jt = &ctx.switchTable->second;
    } else {
        // Check auto-detected jump tables from function analysis
        for (const auto& autoJt : ctx.fn.jumpTables()) {
            if (autoJt.bctrAddress == ctx.base) {
                jt = &autoJt;
                break;
            }
        }
    }

    if (jt)
    {
        ctx.println("\tswitch ({}.u64) {{", ctx.r(jt->indexRegister));

        for (size_t i = 0; i < jt->targets.size(); i++)
        {
            ctx.println("\tcase {}:", i);
            auto label = jt->targets[i];
            if (label < ctx.fn.base() || label >= ctx.fn.end())
            {
                REXCODEGEN_ERROR("Jump target 0x{:08X} outside function bounds at bctr 0x{:08X}", label, ctx.base);
                ctx.println("\t\t// ERROR: jump target 0x{:08X} outside function bounds", label);
                ctx.println("\t\treturn;");
            }
            else
            {
                ctx.println("\t\tgoto loc_{:X};", label);
            }
        }

        ctx.println("\tdefault:");
        ctx.println("\t\t__builtin_trap(); // Switch case out of range");
        ctx.println("\t}}");

        ctx.reset_switch_table();
    }
    else
    {
        // No switch table - assume tail call via CTR
        // NOTE(tomc): If this is actually an unresolved switch table, the code after
        // will be unreachable. This is caught during analysis by discover_blocks.
        // The validation phase will report missing switch tables.
        ctx.println("\tPPC_CALL_INDIRECT_FUNC({}.u32);", ctx.ctr());
        ctx.println("\treturn;");
    }
    return true;
}

bool build_bctrl(BuilderContext& ctx)
{
    if (!ctx.config().skipLr)
        ctx.println("\tctx.lr = 0x{:X};", ctx.base + 4);
    ctx.println("\tPPC_CALL_INDIRECT_FUNC({}.u32);", ctx.ctr());
    ctx.csrState = CSRState::Unknown; // the call could change it
    return true;
}

bool build_bnectr(BuilderContext& ctx)
{
    ctx.println("\tif (!{}.eq) {{", ctx.cr(ctx.insn.operands[0]));
    ctx.println("\t\tPPC_CALL_INDIRECT_FUNC({}.u32);", ctx.ctr());
    ctx.println("\t\treturn;");
    ctx.println("\t}}");
    return true;
}

//=============================================================================
// Decrement Counter and Branch
//=============================================================================

bool build_bdz(BuilderContext& ctx)
{
    ctx.println("\t--{}.u64;", ctx.ctr());
    emitBranchWithBoundsCheck(ctx, ctx.insn.operands[0],
        fmt::format("{}.u32 == 0", ctx.ctr()), "bdz");
    return true;
}

bool build_bdzlr(BuilderContext& ctx)
{
    ctx.println("\t--{}.u64;", ctx.ctr());
    ctx.println("\tif ({}.u32 == 0) return;", ctx.ctr());
    return true;
}

bool build_bdnz(BuilderContext& ctx)
{
    ctx.println("\t--{}.u64;", ctx.ctr());
    emitBranchWithBoundsCheck(ctx, ctx.insn.operands[0],
        fmt::format("{}.u32 != 0", ctx.ctr()), "bdnz");
    return true;
}

bool build_bdnzf(BuilderContext& ctx)
{
    auto bit = crBitName(ctx.insn.operands[0]);
    ctx.println("\t--{}.u64;", ctx.ctr());
    emitBranchWithBoundsCheck(ctx, ctx.insn.operands[1],
        fmt::format("{}.u32 != 0 && !{}.{}", ctx.ctr(), ctx.cr(ctx.insn.operands[0] / 4), bit), "bdnzf");
    return true;
}

bool build_bdnzt(BuilderContext& ctx)
{
    auto bit = crBitName(ctx.insn.operands[0]);
    ctx.println("\t--{}.u64;", ctx.ctr());
    emitBranchWithBoundsCheck(ctx, ctx.insn.operands[1],
        fmt::format("{}.u32 != 0 && {}.{}", ctx.ctr(), ctx.cr(ctx.insn.operands[0] / 4), bit), "bdnzt");
    return true;
}

bool build_bdzf(BuilderContext& ctx)
{
    auto bit = crBitName(ctx.insn.operands[0]);
    ctx.println("\t--{}.u64;", ctx.ctr());
    emitBranchWithBoundsCheck(ctx, ctx.insn.operands[1],
        fmt::format("{}.u32 == 0 && !{}.{}", ctx.ctr(), ctx.cr(ctx.insn.operands[0] / 4), bit), "bdzf");
    return true;
}

//=============================================================================
// Conditional Branch (eq)
//=============================================================================

bool build_beq(BuilderContext& ctx)
{
    ctx.emit_conditional_branch(false, "eq");
    return true;
}

bool build_beqlr(BuilderContext& ctx)
{
    ctx.println("\tif ({}.eq) return;", ctx.cr(ctx.insn.operands[0]));
    return true;
}

bool build_bne(BuilderContext& ctx)
{
    ctx.emit_conditional_branch(true, "eq");
    return true;
}

bool build_bnelr(BuilderContext& ctx)
{
    ctx.println("\tif (!{}.eq) return;", ctx.cr(ctx.insn.operands[0]));
    return true;
}

//=============================================================================
// Conditional Branch (lt)
//=============================================================================

bool build_blt(BuilderContext& ctx)
{
    ctx.emit_conditional_branch(false, "lt");
    return true;
}

bool build_bltlr(BuilderContext& ctx)
{
    ctx.println("\tif ({}.lt) return;", ctx.cr(ctx.insn.operands[0]));
    return true;
}

bool build_bge(BuilderContext& ctx)
{
    ctx.emit_conditional_branch(true, "lt");
    return true;
}

bool build_bgelr(BuilderContext& ctx)
{
    ctx.println("\tif (!{}.lt) return;", ctx.cr(ctx.insn.operands[0]));
    return true;
}

//=============================================================================
// Conditional Branch (gt)
//=============================================================================

bool build_bgt(BuilderContext& ctx)
{
    ctx.emit_conditional_branch(false, "gt");
    return true;
}

bool build_bgtlr(BuilderContext& ctx)
{
    ctx.println("\tif ({}.gt) return;", ctx.cr(ctx.insn.operands[0]));
    return true;
}

bool build_ble(BuilderContext& ctx)
{
    ctx.emit_conditional_branch(true, "gt");
    return true;
}

bool build_blelr(BuilderContext& ctx)
{
    ctx.println("\tif (!{}.gt) return;", ctx.cr(ctx.insn.operands[0]));
    return true;
}

//=============================================================================
// Conditional Branch (so - summary overflow / unordered)
//=============================================================================

bool build_bso(BuilderContext& ctx)
{
    ctx.emit_conditional_branch(false, "so");
    return true;
}

bool build_bsolr(BuilderContext& ctx)
{
    ctx.println("\tif ({}.so) return;", ctx.cr(ctx.insn.operands[0]));
    return true;
}

bool build_bns(BuilderContext& ctx)
{
    ctx.emit_conditional_branch(true, "so");
    return true;
}

bool build_bnslr(BuilderContext& ctx)
{
    ctx.println("\tif (!{}.so) return;", ctx.cr(ctx.insn.operands[0]));
    return true;
}

} // namespace rexglue::codegen::builders
