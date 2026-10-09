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

bool build_cmpd(BuilderContext& ctx) {
  emitCompareRegister(ctx, "int64_t", "s64");
  return true;
}

bool build_cmpdi(BuilderContext& ctx) {
  emitCompareImmediate(ctx, "int64_t", "s64", true);
  return true;
}

bool build_cmpld(BuilderContext& ctx) {
  emitCompareRegister(ctx, "uint64_t", "u64");
  return true;
}

bool build_cmpldi(BuilderContext& ctx) {
  emitCompareImmediate(ctx, "uint64_t", "u64", false);
  return true;
}

bool build_cmplw(BuilderContext& ctx) {
  emitCompareRegister(ctx, "uint32_t", "u32");
  return true;
}

bool build_cmplwi(BuilderContext& ctx) {
  emitCompareImmediate(ctx, "uint32_t", "u32", false);
  return true;
}

bool build_cmpw(BuilderContext& ctx) {
  emitCompareRegister(ctx, "int32_t", "s32");
  return true;
}

bool build_cmpwi(BuilderContext& ctx) {
  emitCompareImmediate(ctx, "int32_t", "s32", true);
  return true;
}

}  // namespace rex::codegen
