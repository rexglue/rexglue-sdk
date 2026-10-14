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
#include "helpers.h"

namespace rex::codegen {

bool BuildCmpd(BuilderContext& ctx) {
  EmitCompareRegister(ctx, "i64", "s64");
  return true;
}

bool BuildCmpdi(BuilderContext& ctx) {
  EmitCompareImmediate(ctx, "i64", "s64", true);
  return true;
}

bool BuildCmpld(BuilderContext& ctx) {
  EmitCompareRegister(ctx, "u64", "u64");
  return true;
}

bool BuildCmpldi(BuilderContext& ctx) {
  EmitCompareImmediate(ctx, "u64", "u64", false);
  return true;
}

bool BuildCmplw(BuilderContext& ctx) {
  EmitCompareRegister(ctx, "u32", "u32");
  return true;
}

bool BuildCmplwi(BuilderContext& ctx) {
  EmitCompareImmediate(ctx, "u32", "u32", false);
  return true;
}

bool BuildCmpw(BuilderContext& ctx) {
  EmitCompareRegister(ctx, "i32", "s32");
  return true;
}

bool BuildCmpwi(BuilderContext& ctx) {
  EmitCompareImmediate(ctx, "i32", "s32", true);
  return true;
}

}  // namespace rex::codegen
