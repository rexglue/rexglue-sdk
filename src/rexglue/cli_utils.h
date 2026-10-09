/**
 * @file        rexglue/cli_utils.h
 * @brief       Shared types for rexglue CLI subcommands
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <functional>

#include <rex/result.h>

namespace rexglue::cli {

struct CLIContext {
  bool verbose = false;
  bool overwrite_existing = false;
  bool generate_despite_errors = false;
};

using DeferredAction = std::move_only_function<rex::Result<void>()>;

}  // namespace rexglue::cli
