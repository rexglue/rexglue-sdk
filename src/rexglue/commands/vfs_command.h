/**
 * @file        rexglue/commands/vfs_command.h
 * @brief       The vfs command, which extracts STFS, SVOD and GDFX containers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "../cli_context.h"

namespace CLI {
class App;
}

namespace rexglue::cli {

/**
 * Registers rexglue vfs dump source destination.
 *
 * @param parent The root command.
 * @param ctx The shared command line state.
 * @param pending Receives the action when the command is chosen.
 */
void RegisterVFS(CLI::App& parent, const CLIContext& ctx, DeferredAction& pending);

}  // namespace rexglue::cli
