/**
 * @file        rexglue/commands/codegen_command.h
 * @brief       Code generation command interface
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "../cli_utils.h"

#include <string>

#include <rex/result.h>

namespace rexglue::cli {

// Import Result from rex namespace
using rex::Result;

/**
 * Generate C++ code from a TOML config file
 * @param config_path Path to TOML config file
 * @param ctx CLI context with template path
 * @return Success or error
 */
Result<void> CodegenFromConfig(const std::string& config_path, const CliContext& ctx);

}  // namespace rexglue::cli
