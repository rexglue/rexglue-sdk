/**
 * @file        codegen/analyze.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/codegen/codegen_context.h>
#include <rex/codegen/progress_reporter.h>
#include <rex/result.h>

namespace rex::codegen {

/**
 * Builds and validates the complete FunctionGraph.
 *
 * Runs the phases in order.
 * - Register imports, helpers, PDATA functions and config functions
 * - Scan code regions (null boundaries) and data regions
 * - Discover function blocks, calls and jump tables, iterating to a fixed point
 * - VTable discovers functions through RTTI vtables
 * - GapFill claims orphaned code regions
 * - Merge resolves all jumps and seals functions
 * - Validate verifies that all calls resolve
 *
 * @param ctx CodegenContext with binary and config loaded
 * @param reporter Optional progress sink, may be null
 * @return Success if the graph is valid, error otherwise
 */
[[nodiscard]] Result<void> Analyze(CodegenContext& ctx, ProgressReporter* reporter = nullptr);

}  // namespace rex::codegen
