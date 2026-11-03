/**
 * @file        codegen/builder/comparison.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "builder.h"

#include <dis-asm.h>
#include <ppc-inst.h>

namespace rex::codegen {

namespace {

/** Emits `crD.Compare<T>(rA.field, rB.field, XER)` for the register forms. */
void EmitCompareRegister(BuilderContext& ctx, const char* type_name, const char* field) {
  ctx.Println("\t{}.Compare<{}>({}.{}, {}.{}, {});", ctx.CR(ctx.insn.operands[0]), type_name,
              ctx.R(ctx.insn.operands[1]), field, ctx.R(ctx.insn.operands[2]), field, ctx.XER());
}

/** Emits `crD.Compare<T>(rA.field, imm, XER)` for the immediate forms. */
void EmitCompareImmediate(BuilderContext& ctx, const char* type_name, const char* field,
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

}  // namespace

template <>
bool Build<PPC_INST_CMPD>(BuilderContext& ctx) {
  EmitCompareRegister(ctx, "i64", "s64");
  return true;
}

template <>
bool Build<PPC_INST_CMPDI>(BuilderContext& ctx) {
  EmitCompareImmediate(ctx, "i64", "s64", true);
  return true;
}

template <>
bool Build<PPC_INST_CMPLD>(BuilderContext& ctx) {
  EmitCompareRegister(ctx, "u64", "u64");
  return true;
}

template <>
bool Build<PPC_INST_CMPLDI>(BuilderContext& ctx) {
  EmitCompareImmediate(ctx, "u64", "u64", false);
  return true;
}

template <>
bool Build<PPC_INST_CMPLW>(BuilderContext& ctx) {
  EmitCompareRegister(ctx, "u32", "u32");
  return true;
}

template <>
bool Build<PPC_INST_CMPLWI>(BuilderContext& ctx) {
  EmitCompareImmediate(ctx, "u32", "u32", false);
  return true;
}

template <>
bool Build<PPC_INST_CMPW>(BuilderContext& ctx) {
  EmitCompareRegister(ctx, "i32", "s32");
  return true;
}

template <>
bool Build<PPC_INST_CMPWI>(BuilderContext& ctx) {
  EmitCompareImmediate(ctx, "i32", "s32", true);
  return true;
}

}  // namespace rex::codegen
