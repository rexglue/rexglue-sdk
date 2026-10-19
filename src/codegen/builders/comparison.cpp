/**
 * @file        codegen/builders/comparison.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"

#include <dis-asm.h>
#include <ppc-inst.h>

namespace rex::codegen {

namespace {

/**
 * Emits a comparison of 2 registers.
 *
 * Pattern `crD.Compare<T>(rA.field, rB.field, XER)`
 * Used by cmpd, cmpld, cmplw, cmpw
 *
 * @param ctx The builder context
 * @param type_name The comparison type (e.g., "i64", "u32")
 * @param field The register field accessor (e.g., "s64", "u32")
 */
void EmitCompareRegister(BuilderContext& ctx, const char* type_name, const char* field) {
  ctx.Println("\t{}.Compare<{}>({}.{}, {}.{}, {});", ctx.CR(ctx.insn.operands[0]), type_name,
              ctx.R(ctx.insn.operands[1]), field, ctx.R(ctx.insn.operands[2]), field, ctx.XER());
}

/**
 * Emits a comparison of a register and an immediate.
 *
 * Pattern `crD.Compare<T>(rA.field, imm, XER)`
 * Used by cmpdi, cmpldi, cmplwi, cmpwi
 *
 * @param ctx The builder context
 * @param type_name The comparison type (e.g., "i64", "u32")
 * @param field The register field accessor (e.g., "s64", "u32")
 * @param sign_extend If true, sign-extend the immediate via static_cast<i32>
 */
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

bool BuildCmpd(BuilderContext& ctx) {
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
