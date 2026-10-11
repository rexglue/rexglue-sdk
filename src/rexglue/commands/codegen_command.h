/**
 * @file        rexglue/commands/codegen_command.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "../cli_context.h"

#include <string>
#include <vector>

#include <rex/codegen/limits.h>
#include <rex/result.h>

namespace CLI {
class App;
}

namespace rexglue::cli {

using rex::Result;

[[nodiscard]] Result<void> CodegenFromConfig(const std::string& config_path, const CLIContext& ctx,
                                             const std::vector<std::string>& targets,
                                             bool ignore_stamp = false,
                                             const rex::codegen::Limits& limits = {});

[[nodiscard]] Result<std::string> DiscoverManifestInCwd();

void RegisterCodegen(CLI::App& parent, const CLIContext& ctx, DeferredAction& pending);

}  // namespace rexglue::cli
