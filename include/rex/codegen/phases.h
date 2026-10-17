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

namespace rex::codegen::detail {

/**
 * Registers PDATA, CONFIG, helpers and imports into FunctionGraph.
 *
 * @param ctx CodegenContext with binary and config loaded
 * @param reporter Optional progress sink, may be null
 * @return Success, or the failing phase error
 */
[[nodiscard]] Result<void> Register(CodegenContext& ctx, IProgressReporter* reporter = nullptr);

/**
 * Scans the binary for bl targets, thunks and bctr locations.
 *
 * @param ctx CodegenContext with binary and config loaded
 * @param reporter Optional progress sink, may be null
 * @return Success, or the failing phase error
 */
[[nodiscard]] Result<void> Scan(CodegenContext& ctx, IProgressReporter* reporter = nullptr);

/**
 * Discovers function blocks from candidates.
 *
 * @param ctx CodegenContext with binary and config loaded
 * @param reporter Optional progress sink, may be null
 * @return Success, or the failing phase error
 */
[[nodiscard]] Result<void> Discover(CodegenContext& ctx, IProgressReporter* reporter = nullptr);

/**
 * Expands and seals functions by vacancy.
 *
 * @param ctx CodegenContext with binary and config loaded
 * @param reporter Optional progress sink, may be null
 * @return Success, or the failing phase error
 */
[[nodiscard]] Result<void> Merge(CodegenContext& ctx, IProgressReporter* reporter = nullptr);

/**
 * Finds uncovered code regions and registers them as functions.
 *
 * @param ctx CodegenContext with binary and config loaded
 * @param reporter Optional progress sink, may be null
 * @return Success, or the failing phase error
 */
[[nodiscard]] Result<void> GapFill(CodegenContext& ctx, IProgressReporter* reporter = nullptr);

/**
 * Validates that all call targets resolve.
 *
 * @param ctx CodegenContext with binary and config loaded
 * @param reporter Optional progress sink, may be null
 * @return Success, or the failing phase error
 */
[[nodiscard]] Result<void> Validate(CodegenContext& ctx, IProgressReporter* reporter = nullptr);

}  // namespace rex::codegen::detail
