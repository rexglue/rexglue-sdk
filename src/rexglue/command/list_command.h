/**
 * @file        rexglue/command/list_command.h
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
 * Registers the list shim command, which lists the functions a title can replace with a shim.
 *
 * @param pending Receives the action when the command is chosen.
 */
void RegisterList(CLI::App& parent, const CLIContext& ctx, DeferredAction& pending);

}  // namespace rexglue::cli
