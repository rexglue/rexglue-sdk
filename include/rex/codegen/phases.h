/**
 * @file        codegen/phases.h
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
#include <rex/codegen/progress_reporter.h>
#include <rex/result.h>

namespace rex::codegen::phases {

/// Register PDATA, CONFIG, helpers, imports into FunctionGraph
[[nodiscard]] Result<void> Register(CodegenContext& ctx, ProgressReporter* reporter = nullptr);

/// Scan binary for bl targets, thunks, bctr locations
[[nodiscard]] Result<void> Scan(CodegenContext& ctx, ProgressReporter* reporter = nullptr);

/// Discover function blocks from candidates
[[nodiscard]] Result<void> Discover(CodegenContext& ctx, ProgressReporter* reporter = nullptr);

/// Vacancy-based function expansion and sealing
[[nodiscard]] Result<void> Merge(CodegenContext& ctx, ProgressReporter* reporter = nullptr);

/// Find uncovered code regions and register them as functions
[[nodiscard]] Result<void> GapFill(CodegenContext& ctx, ProgressReporter* reporter = nullptr);

/// Validate all call targets resolve
[[nodiscard]] Result<void> Validate(CodegenContext& ctx, ProgressReporter* reporter = nullptr);

}  // namespace rex::codegen::phases
