/**
 * @file        rex/codegen/analyze.h
 * @brief       Function graph analysis (builds + validates)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/codegen/codegen_context.h>
#include <rex/result.h>

namespace rex::codegen {

/**
 * Build and validate the complete FunctionGraph.
 *
 * This function consolidates all analysis phases:
 * - Register: imports, helpers, PDATA functions, config functions
 * - Scan: code regions (null boundaries), data regions
 * - Discover: function blocks, calls, jump tables (iterative to fixed point)
 * - VTable: discover functions via RTTI vtables
 * - GapFill: claim orphaned code regions
 * - Merge: resolve all jumps, seal functions
 * - Validate: verify all calls resolve
 *
 * @param ctx CodegenContext with binary and config loaded
 * @return Success if graph is valid, error otherwise
 */
Result<void> Analyze(CodegenContext& ctx);

}  // namespace rex::codegen
