/**
 * @file        rexglue/cli_utils.h
 * @brief       CLI utility functions and helpers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <string>

namespace rexglue::cli {

/**
 * Shared CLI context passed to command handlers
 */
struct CliContext {
    bool verbose = false;
    bool force = false;  // Generate output despite validation errors
    bool enableExceptionHandlers = false;  // Enable SEH exception handler generation
};

} // namespace rexglue::cli
