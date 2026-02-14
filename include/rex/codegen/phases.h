/**
 * @file        rexcodegen/phases.h
 * @brief       Analysis phase entry points
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>

#include <rex/codegen/codegen_context.h>
#include <rex/result.h>

namespace rex::codegen::phases {

/// Register PDATA, CONFIG, helpers, imports into FunctionGraph
VoidResult Register(CodegenContext& ctx);

/// Scan binary for bl targets, thunks, bctr locations
VoidResult Scan(CodegenContext& ctx);

/// Discover function blocks from candidates
VoidResult Discover(CodegenContext& ctx);

/// Vacancy-based function expansion and sealing
VoidResult Merge(CodegenContext& ctx);

/// Find uncovered code regions and register them as functions
VoidResult GapFill(CodegenContext& ctx);

/// Validate all call targets resolve
VoidResult Validate(CodegenContext& ctx);

}  // namespace rex::codegen::phases
