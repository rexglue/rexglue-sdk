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

bool BuildB(BuilderContext& ctx) {
  u32 target = ctx.insn.operands[0];

  auto kind = ctx.GetGraph().ClassifyTarget(target, ctx.base, false);

  switch (kind) {
    case TargetKind::kInternalLabel:
      ctx.Println("\tgoto loc_{:X};", target);
      break;

    case TargetKind::kFunction:
    case TargetKind::kImport:
      ctx.EmitFunctionCall(target);
      ctx.Println("\treturn;");
      break;

    case TargetKind::kUnknown:
      if (target >= ctx.fn.GetBase() && target < ctx.fn.GetEnd()) {
        ctx.Println("\tgoto loc_{:X};", target);
      } else {
        REXLOG_WARN(codegen, "Unresolved b target 0x{:08X} from 0x{:08X}", target, ctx.base);
        ctx.EmitFunctionCall(target);
        ctx.Println("\treturn;");
      }
      break;
  }
  return true;
}

bool BuildBl(BuilderContext& ctx) {
  u32 target = ctx.insn.operands[0];

  if (!ctx.GetConfig().skip_lr)
    ctx.Println("\tctx.lr = 0x{:X};", ctx.base + 4);

  auto kind = ctx.GetGraph().ClassifyTarget(target, ctx.base, true);

  switch (kind) {
    case TargetKind::kInternalLabel:
      ctx.Println("\tgoto loc_{:X};", target);
      break;

    case TargetKind::kFunction:
    case TargetKind::kImport:
      ctx.EmitFunctionCall(target);
      ctx.csr_state = CSRState::kUnknown;
      break;

    case TargetKind::kUnknown:
      REXLOG_ERROR(codegen, "Unresolved bl target 0x{:08X} from 0x{:08X}", target, ctx.base);
      ctx.Println("\t// ERROR: unresolved bl target 0x{:08X}", target);
      ctx.Println("\tREX_FATAL(cpu, \"Unresolved call from 0x{:08X} to 0x{:08X}\");", ctx.base,
                  target);
      break;
  }
  return true;
}

bool BuildBlr(BuilderContext& ctx) {
  ctx.Println("\treturn;");
  return true;
}

bool BuildBlrl(BuilderContext& ctx) {
  ctx.Println("\t{{ auto old_lr = ctx.lr;");
  if (!ctx.GetConfig().skip_lr)
    ctx.Println("\tctx.lr = 0x{:X};", ctx.base + 4);
  ctx.Println("\tREX_CALL_INDIRECT_FUNC(u32(old_lr)); }}");
  ctx.csr_state = CSRState::kUnknown;
  return true;
}

bool BuildBctr(BuilderContext& ctx) {
  const JumpTable* jt = ctx.active_jump_table;

  if (!jt) {
    for (const auto& auto_jt : ctx.fn.GetJumpTables()) {
      if (auto_jt.bctr_address == ctx.base) {
        jt = &auto_jt;
        break;
      }
    }
  }

  if (jt) {
    ctx.Println("\tswitch ({}.u32) {{", ctx.R(jt->index_register));

    for (size_t i = 0; i < jt->targets.size(); i++) {
      ctx.Println("\tcase {}:", i);
      auto label = jt->targets[i];

      if (label == 0) {
        ctx.Println("\t\t__builtin_trap(); // ERROR - detected jump to null value");
        continue;
      }

      auto kind = ctx.GetGraph().ClassifyTarget(label, ctx.base, false);
      switch (kind) {
        case TargetKind::kInternalLabel:
          ctx.Println("\t\tgoto loc_{:X};", label);
          break;
        case TargetKind::kFunction:
        case TargetKind::kImport:
          if (auto* target_fn = ctx.GetGraph().GetFunction(label)) {
            ctx.emit_ctx.Reference(target_fn->GetName());
            ctx.Println("\t\t{}(ctx, base);", target_fn->GetName());
          } else {
            REXLOG_ERROR(
                codegen,
                "Jump target 0x{:08X} classified as function but not in graph at bctr 0x{:08X}",
                label, ctx.base);
            ctx.Println(
                "\t\tREX_FATAL(cpu, \"Jump target 0x{:08X} classified as function but not "
                "in graph at bctr 0x{:08X}\");",
                label, ctx.base);
          }
          ctx.Println("\t\treturn;");
          break;
        default:
          REXLOG_ERROR(codegen, "Jump target 0x{:08X} unresolved at bctr 0x{:08X}", label,
                       ctx.base);
          ctx.Println("\t\tREX_FATAL(cpu, \"Jump target 0x{:08X} unresolved at bctr 0x{:08X}\");",
                      label, ctx.base);
          break;
      }
    }

    ctx.Println("\tdefault:");
    ctx.Println("\t\t__builtin_trap(); // Switch case out of range");
    ctx.Println("\t}}");

    ctx.GetResetSwitchTable();
  } else {
    ctx.Println("\tREX_CALL_INDIRECT_FUNC({}.u32);", ctx.CTR());
    ctx.Println("\treturn;");
  }
  return true;
}

bool BuildBctrl(BuilderContext& ctx) {
  if (!ctx.GetConfig().skip_lr)
    ctx.Println("\tctx.lr = 0x{:X};", ctx.base + 4);
  ctx.Println("\tREX_CALL_INDIRECT_FUNC({}.u32);", ctx.CTR());
  ctx.csr_state = CSRState::kUnknown;
  return true;
}

bool BuildBnectr(BuilderContext& ctx) {
  ctx.Println("\tif (!{}.eq) {{", ctx.CR(ctx.insn.operands[0]));
  ctx.Println("\t\tREX_CALL_INDIRECT_FUNC({}.u32);", ctx.CTR());
  ctx.Println("\t\treturn;");
  ctx.Println("\t}}");
  return true;
}

bool BuildBdz(BuilderContext& ctx) {
  ctx.Println("\t--{}.u64;", ctx.CTR());
  EmitBranchWithBoundsCheck(ctx, ctx.insn.operands[0], std::format("{}.u32 == 0", ctx.CTR()),
                            "bdz");
  return true;
}

bool BuildBdzlr(BuilderContext& ctx) {
  ctx.Println("\t--{}.u64;", ctx.CTR());
  ctx.Println("\tif ({}.u32 == 0) return;", ctx.CTR());
  return true;
}

bool BuildBdnzlr(BuilderContext& ctx) {
  ctx.Println("\t--{}.u64;", ctx.CTR());
  ctx.Println("\tif ({}.u32 != 0) return;", ctx.CTR());
  return true;
}

bool BuildBdnz(BuilderContext& ctx) {
  ctx.Println("\t--{}.u64;", ctx.CTR());
  EmitBranchWithBoundsCheck(ctx, ctx.insn.operands[0], std::format("{}.u32 != 0", ctx.CTR()),
                            "bdnz");
  return true;
}

bool BuildBdnzf(BuilderContext& ctx) {
  auto bit = GetCRBitName(ctx.insn.operands[0]);
  ctx.Println("\t--{}.u64;", ctx.CTR());
  EmitBranchWithBoundsCheck(
      ctx, ctx.insn.operands[1],
      std::format("{}.u32 != 0 && !{}.{}", ctx.CTR(), ctx.CR(ctx.insn.operands[0] / 4), bit),
      "bdnzf");
  return true;
}

bool BuildBdnzt(BuilderContext& ctx) {
  auto bit = GetCRBitName(ctx.insn.operands[0]);
  ctx.Println("\t--{}.u64;", ctx.CTR());
  EmitBranchWithBoundsCheck(
      ctx, ctx.insn.operands[1],
      std::format("{}.u32 != 0 && {}.{}", ctx.CTR(), ctx.CR(ctx.insn.operands[0] / 4), bit),
      "bdnzt");
  return true;
}

bool BuildBdzf(BuilderContext& ctx) {
  auto bit = GetCRBitName(ctx.insn.operands[0]);
  ctx.Println("\t--{}.u64;", ctx.CTR());
  EmitBranchWithBoundsCheck(
      ctx, ctx.insn.operands[1],
      std::format("{}.u32 == 0 && !{}.{}", ctx.CTR(), ctx.CR(ctx.insn.operands[0] / 4), bit),
      "bdzf");
  return true;
}

bool BuildBeq(BuilderContext& ctx) {
  ctx.EmitConditionalBranch(false, "eq");
  return true;
}

bool BuildBeqlr(BuilderContext& ctx) {
  ctx.Println("\tif ({}.eq) return;", ctx.CR(ctx.insn.operands[0]));
  return true;
}

bool BuildBne(BuilderContext& ctx) {
  ctx.EmitConditionalBranch(true, "eq");
  return true;
}

bool BuildBnelr(BuilderContext& ctx) {
  ctx.Println("\tif (!{}.eq) return;", ctx.CR(ctx.insn.operands[0]));
  return true;
}

bool BuildBlt(BuilderContext& ctx) {
  ctx.EmitConditionalBranch(false, "lt");
  return true;
}

bool BuildBltlr(BuilderContext& ctx) {
  ctx.Println("\tif ({}.lt) return;", ctx.CR(ctx.insn.operands[0]));
  return true;
}

bool BuildBge(BuilderContext& ctx) {
  ctx.EmitConditionalBranch(true, "lt");
  return true;
}

bool BuildBgelr(BuilderContext& ctx) {
  ctx.Println("\tif (!{}.lt) return;", ctx.CR(ctx.insn.operands[0]));
  return true;
}

bool BuildBgt(BuilderContext& ctx) {
  ctx.EmitConditionalBranch(false, "gt");
  return true;
}

bool BuildBgtlr(BuilderContext& ctx) {
  ctx.Println("\tif ({}.gt) return;", ctx.CR(ctx.insn.operands[0]));
  return true;
}

bool BuildBle(BuilderContext& ctx) {
  ctx.EmitConditionalBranch(true, "gt");
  return true;
}

bool BuildBlelr(BuilderContext& ctx) {
  ctx.Println("\tif (!{}.gt) return;", ctx.CR(ctx.insn.operands[0]));
  return true;
}

bool BuildBso(BuilderContext& ctx) {
  ctx.EmitConditionalBranch(false, "so");
  return true;
}

bool BuildBsolr(BuilderContext& ctx) {
  ctx.Println("\tif ({}.so) return;", ctx.CR(ctx.insn.operands[0]));
  return true;
}

bool BuildBns(BuilderContext& ctx) {
  ctx.EmitConditionalBranch(true, "so");
  return true;
}

bool BuildBnslr(BuilderContext& ctx) {
  ctx.Println("\tif (!{}.so) return;", ctx.CR(ctx.insn.operands[0]));
  return true;
}

}  // namespace rex::codegen
