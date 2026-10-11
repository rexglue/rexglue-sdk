/**
 * @file        rexglue/commands/list_command.h
 * @brief       The list command, which prints what the SDK provides to a title config
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
 * Registers rexglue list shim.
 *
 * @param parent The root command.
 * @param ctx The shared command line state.
 * @param pending Receives the action when the command is chosen.
 */
void RegisterList(CLI::App& parent, const CLIContext& ctx, DeferredAction& pending);

}  // namespace rexglue::cli
