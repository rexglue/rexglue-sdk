/**
 * @file        codegen/builders/control_flow.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "helpers.h"

#include <format>

#include <rex/logging.h>

namespace rex::codegen {

bool build_b(BuilderContext& ctx) {
  u32 target = ctx.insn.operands[0];

  auto kind = ctx.graph().classifyTarget(target, ctx.base, false);

  switch (kind) {
    case TargetKind::InternalLabel:
      ctx.println("\tgoto loc_{:X};", target);
      break;

    case TargetKind::Function:
    case TargetKind::Import:
      ctx.emit_function_call(target);
      ctx.println("\treturn;");
      break;

    case TargetKind::Unknown:
      if (target >= ctx.fn.base() && target < ctx.fn.end()) {
        ctx.println("\tgoto loc_{:X};", target);
      } else {
        REXLOG_WARN(codegen, "Unresolved b target 0x{:08X} from 0x{:08X}", target, ctx.base);
        ctx.emit_function_call(target);
        ctx.println("\treturn;");
      }
      break;
  }
  return true;
}

bool build_bl(BuilderContext& ctx) {
  u32 target = ctx.insn.operands[0];

  if (!ctx.config().skipLr)
    ctx.println("\tctx.lr = 0x{:X};", ctx.base + 4);

  auto kind = ctx.graph().classifyTarget(target, ctx.base, true);

  switch (kind) {
    case TargetKind::InternalLabel:
      ctx.println("\tgoto loc_{:X};", target);
      break;

    case TargetKind::Function:
    case TargetKind::Import:
      ctx.emit_function_call(target);
      ctx.csrState = CSRState::Unknown;
      break;

    case TargetKind::Unknown:
      REXLOG_ERROR(codegen, "Unresolved bl target 0x{:08X} from 0x{:08X}", target, ctx.base);
      ctx.println("\t// ERROR: unresolved bl target 0x{:08X}", target);
      ctx.println("\tREX_FATAL(cpu, \"Unresolved call from 0x{:08X} to 0x{:08X}\");", ctx.base,
                  target);
      break;
  }
  return true;
}

bool build_blr(BuilderContext& ctx) {
  ctx.println("\treturn;");
  return true;
}

bool build_blrl(BuilderContext& ctx) {
  ctx.println("\t{{ auto old_lr = ctx.lr;");
  if (!ctx.config().skipLr)
    ctx.println("\tctx.lr = 0x{:X};", ctx.base + 4);
  ctx.println("\tREX_CALL_INDIRECT_FUNC(u32(old_lr)); }}");
  ctx.csrState = CSRState::Unknown;
  return true;
}

bool build_bctr(BuilderContext& ctx) {
  const JumpTable* jt = ctx.activeJumpTable;

  if (!jt) {
    for (const auto& autoJt : ctx.fn.jumpTables()) {
      if (autoJt.bctrAddress == ctx.base) {
        jt = &autoJt;
        break;
      }
    }
  }

  if (jt) {
    ctx.println("\tswitch ({}.u32) {{", ctx.r(jt->indexRegister));

    for (size_t i = 0; i < jt->targets.size(); i++) {
      ctx.println("\tcase {}:", i);
      auto label = jt->targets[i];

      if (label == 0) {
        ctx.println("\t\t__builtin_trap(); // ERROR - detected jump to null value");
        continue;
      }

      auto kind = ctx.graph().classifyTarget(label, ctx.base, false);
      switch (kind) {
        case TargetKind::InternalLabel:
          ctx.println("\t\tgoto loc_{:X};", label);
          break;
        case TargetKind::Function:
        case TargetKind::Import:
          if (auto* targetFn = ctx.graph().getFunction(label)) {
            ctx.emitCtx.reference(targetFn->name());
            ctx.println("\t\t{}(ctx, base);", targetFn->name());
          } else {
            REXLOG_ERROR(
                codegen,
                "Jump target 0x{:08X} classified as function but not in graph at bctr 0x{:08X}",
                label, ctx.base);
            ctx.println(
                "\t\tREX_FATAL(cpu, \"Jump target 0x{:08X} classified as function but not "
                "in graph at bctr 0x{:08X}\");",
                label, ctx.base);
          }
          ctx.println("\t\treturn;");
          break;
        default:
          REXLOG_ERROR(codegen, "Jump target 0x{:08X} unresolved at bctr 0x{:08X}", label,
                       ctx.base);
          ctx.println("\t\tREX_FATAL(cpu, \"Jump target 0x{:08X} unresolved at bctr 0x{:08X}\");",
                      label, ctx.base);
          break;
      }
    }

    ctx.println("\tdefault:");
    ctx.println("\t\t__builtin_trap(); // Switch case out of range");
    ctx.println("\t}}");

    ctx.reset_switch_table();
  } else {
    ctx.println("\tREX_CALL_INDIRECT_FUNC({}.u32);", ctx.ctr());
    ctx.println("\treturn;");
  }
  return true;
}

bool build_bctrl(BuilderContext& ctx) {
  if (!ctx.config().skipLr)
    ctx.println("\tctx.lr = 0x{:X};", ctx.base + 4);
  ctx.println("\tREX_CALL_INDIRECT_FUNC({}.u32);", ctx.ctr());
  ctx.csrState = CSRState::Unknown;
  return true;
}

bool build_bnectr(BuilderContext& ctx) {
  ctx.println("\tif (!{}.eq) {{", ctx.cr(ctx.insn.operands[0]));
  ctx.println("\t\tREX_CALL_INDIRECT_FUNC({}.u32);", ctx.ctr());
  ctx.println("\t\treturn;");
  ctx.println("\t}}");
  return true;
}

bool build_bdz(BuilderContext& ctx) {
  ctx.println("\t--{}.u64;", ctx.ctr());
  emitBranchWithBoundsCheck(ctx, ctx.insn.operands[0], std::format("{}.u32 == 0", ctx.ctr()),
                            "bdz");
  return true;
}

bool build_bdzlr(BuilderContext& ctx) {
  ctx.println("\t--{}.u64;", ctx.ctr());
  ctx.println("\tif ({}.u32 == 0) return;", ctx.ctr());
  return true;
}

bool build_bdnzlr(BuilderContext& ctx) {
  ctx.println("\t--{}.u64;", ctx.ctr());
  ctx.println("\tif ({}.u32 != 0) return;", ctx.ctr());
  return true;
}

bool build_bdnz(BuilderContext& ctx) {
  ctx.println("\t--{}.u64;", ctx.ctr());
  emitBranchWithBoundsCheck(ctx, ctx.insn.operands[0], std::format("{}.u32 != 0", ctx.ctr()),
                            "bdnz");
  return true;
}

bool build_bdnzf(BuilderContext& ctx) {
  auto bit = crBitName(ctx.insn.operands[0]);
  ctx.println("\t--{}.u64;", ctx.ctr());
  emitBranchWithBoundsCheck(
      ctx, ctx.insn.operands[1],
      std::format("{}.u32 != 0 && !{}.{}", ctx.ctr(), ctx.cr(ctx.insn.operands[0] / 4), bit),
      "bdnzf");
  return true;
}

bool build_bdnzt(BuilderContext& ctx) {
  auto bit = crBitName(ctx.insn.operands[0]);
  ctx.println("\t--{}.u64;", ctx.ctr());
  emitBranchWithBoundsCheck(
      ctx, ctx.insn.operands[1],
      std::format("{}.u32 != 0 && {}.{}", ctx.ctr(), ctx.cr(ctx.insn.operands[0] / 4), bit),
      "bdnzt");
  return true;
}

bool build_bdzf(BuilderContext& ctx) {
  auto bit = crBitName(ctx.insn.operands[0]);
  ctx.println("\t--{}.u64;", ctx.ctr());
  emitBranchWithBoundsCheck(
      ctx, ctx.insn.operands[1],
      std::format("{}.u32 == 0 && !{}.{}", ctx.ctr(), ctx.cr(ctx.insn.operands[0] / 4), bit),
      "bdzf");
  return true;
}

bool build_beq(BuilderContext& ctx) {
  ctx.emit_conditional_branch(false, "eq");
  return true;
}

bool build_beqlr(BuilderContext& ctx) {
  ctx.println("\tif ({}.eq) return;", ctx.cr(ctx.insn.operands[0]));
  return true;
}

bool build_bne(BuilderContext& ctx) {
  ctx.emit_conditional_branch(true, "eq");
  return true;
}

bool build_bnelr(BuilderContext& ctx) {
  ctx.println("\tif (!{}.eq) return;", ctx.cr(ctx.insn.operands[0]));
  return true;
}

bool build_blt(BuilderContext& ctx) {
  ctx.emit_conditional_branch(false, "lt");
  return true;
}

bool build_bltlr(BuilderContext& ctx) {
  ctx.println("\tif ({}.lt) return;", ctx.cr(ctx.insn.operands[0]));
  return true;
}

bool build_bge(BuilderContext& ctx) {
  ctx.emit_conditional_branch(true, "lt");
  return true;
}

bool build_bgelr(BuilderContext& ctx) {
  ctx.println("\tif (!{}.lt) return;", ctx.cr(ctx.insn.operands[0]));
  return true;
}

bool build_bgt(BuilderContext& ctx) {
  ctx.emit_conditional_branch(false, "gt");
  return true;
}

bool build_bgtlr(BuilderContext& ctx) {
  ctx.println("\tif ({}.gt) return;", ctx.cr(ctx.insn.operands[0]));
  return true;
}

bool build_ble(BuilderContext& ctx) {
  ctx.emit_conditional_branch(true, "gt");
  return true;
}

bool build_blelr(BuilderContext& ctx) {
  ctx.println("\tif (!{}.gt) return;", ctx.cr(ctx.insn.operands[0]));
  return true;
}

bool build_bso(BuilderContext& ctx) {
  ctx.emit_conditional_branch(false, "so");
  return true;
}

bool build_bsolr(BuilderContext& ctx) {
  ctx.println("\tif ({}.so) return;", ctx.cr(ctx.insn.operands[0]));
  return true;
}

bool build_bns(BuilderContext& ctx) {
  ctx.emit_conditional_branch(true, "so");
  return true;
}

bool build_bnslr(BuilderContext& ctx) {
  ctx.println("\tif (!{}.so) return;", ctx.cr(ctx.insn.operands[0]));
  return true;
}

}  // namespace rex::codegen
