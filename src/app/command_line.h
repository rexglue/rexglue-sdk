/**
 * @file        app/command_line.h
 * @brief       Reads the command line into the settings registry
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <span>
#include <string>
#include <vector>

#include <rex/cli.h>
#include <rex/platform.h>
#include <rex/result.h>

namespace rex::app::detail {

/**
 * Applies command line options to the settings registry at the command line layer.
 * An option named like a declared action goes to hits, not to a setting.
 * --scope.name=value and --scope.name set a setting, --no-scope.name sets it false.
 * --game_data_root, --user_data_root, --update_data_root, --cache_root, --metadata_root and
 * --config_file set the matching setting. --modules sets nothing and logs a warning.
 *
 * @param args The parsed command line.
 * @param actions The actions the app declares.
 * @param hits Receives each declared action found, in command line order. May be null.
 * @return Ok, or a Config error for unknown options or a setting that refuses its value.
 */
[[nodiscard]] Result<void> ParseCommandLine(const CLIArgList& args,
                                            std::span<const CLIAction> actions = {},
                                            std::vector<CLIActionHit>* hits = nullptr);

/**
 * Checks the actions an app declares.
 *
 * @param actions The declared actions.
 * @return Ok, or a Config error for a repeated name, an invalid name or a name shared with a flag.
 */
[[nodiscard]] Result<void> ValidateCLIActions(std::span<const CLIAction> actions);

}  // namespace rex::app::detail
