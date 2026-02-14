/**
 * @file        rexcodegen/builders/comparison.cpp
 * @brief       PPC comparison instruction code generation
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
// Signed 64-bit Comparisons
//=============================================================================

bool build_cmpd(BuilderContext& ctx) {
  ctx.println("\t{}.compare<int64_t>({}.s64, {}.s64, {});", ctx.cr(ctx.insn.operands[0]),
              ctx.r(ctx.insn.operands[1]), ctx.r(ctx.insn.operands[2]), ctx.xer());
  return true;
}

bool build_cmpdi(BuilderContext& ctx) {
  ctx.println("\t{}.compare<int64_t>({}.s64, {}, {});", ctx.cr(ctx.insn.operands[0]),
              ctx.r(ctx.insn.operands[1]), static_cast<int32_t>(ctx.insn.operands[2]), ctx.xer());
  return true;
}

//=============================================================================
// Unsigned 64-bit Comparisons
//=============================================================================

bool build_cmpld(BuilderContext& ctx) {
  ctx.println("\t{}.compare<uint64_t>({}.u64, {}.u64, {});", ctx.cr(ctx.insn.operands[0]),
              ctx.r(ctx.insn.operands[1]), ctx.r(ctx.insn.operands[2]), ctx.xer());
  return true;
}

bool build_cmpldi(BuilderContext& ctx) {
  ctx.println("\t{}.compare<uint64_t>({}.u64, {}, {});", ctx.cr(ctx.insn.operands[0]),
              ctx.r(ctx.insn.operands[1]), ctx.insn.operands[2], ctx.xer());
  return true;
}

//=============================================================================
// Unsigned 32-bit Comparisons
//=============================================================================

bool build_cmplw(BuilderContext& ctx) {
  ctx.println("\t{}.compare<uint32_t>({}.u32, {}.u32, {});", ctx.cr(ctx.insn.operands[0]),
              ctx.r(ctx.insn.operands[1]), ctx.r(ctx.insn.operands[2]), ctx.xer());
  return true;
}

bool build_cmplwi(BuilderContext& ctx) {
  ctx.println("\t{}.compare<uint32_t>({}.u32, {}, {});", ctx.cr(ctx.insn.operands[0]),
              ctx.r(ctx.insn.operands[1]), ctx.insn.operands[2], ctx.xer());
  return true;
}

//=============================================================================
// Signed 32-bit Comparisons
//=============================================================================

bool build_cmpw(BuilderContext& ctx) {
  ctx.println("\t{}.compare<int32_t>({}.s32, {}.s32, {});", ctx.cr(ctx.insn.operands[0]),
              ctx.r(ctx.insn.operands[1]), ctx.r(ctx.insn.operands[2]), ctx.xer());
  return true;
}

bool build_cmpwi(BuilderContext& ctx) {
  ctx.println("\t{}.compare<int32_t>({}.s32, {}, {});", ctx.cr(ctx.insn.operands[0]),
              ctx.r(ctx.insn.operands[1]), static_cast<int32_t>(ctx.insn.operands[2]), ctx.xer());
  return true;
}

}  // namespace rex::codegen
