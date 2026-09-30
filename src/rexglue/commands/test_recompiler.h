/**
 * @file        rexglue/commands/test_recompiler.h
 * @brief       Recompiler test command interface
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#pragma once

#include "../cli_utils.h"

namespace CLI {
class App;
}

namespace rexglue::cli {

void RegisterRecompileTests(CLI::App& parent, const CLIContext& ctx, DeferredAction& pending);

}  // namespace rexglue::cli
