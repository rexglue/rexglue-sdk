/**
 * @file        rex/codegen/recompile.h
 * @brief       C++ code generation from FunctionGraph
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
 * Generate C++ code from validated FunctionGraph.
 *
 * Writes output files to ctx.Config().outDirectoryPath:
 * - Multiple .cpp files (500 functions each)
 * - Header file with declarations
 * - Function table initialization
 * - Config definitions
 *
 * @param ctx CodegenContext with analyzed FunctionGraph
 * @param force If true, generate output even with validation errors
 * @return Success or error
 */
Result<void> Recompile(CodegenContext& ctx, bool force = false);

}  // namespace rex::codegen
