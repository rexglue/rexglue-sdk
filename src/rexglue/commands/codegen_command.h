/**
 * @file        rexglue/commands/codegen_command.h
 * @brief       Code generation command interface
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#pragma once

#include "../cli_utils.h"

#include <string>
#include <vector>

#include <rex/codegen/limits.h>
#include <rex/result.h>

namespace CLI {
class App;
}

namespace rexglue::cli {

using rex::Result;

Result<void> CodegenFromConfig(const std::string& config_path, const CliContext& ctx,
                               const std::vector<std::string>& targets, bool ignore_stamp = false,
                               const rex::codegen::Limits& limits = {});

Result<std::string> DiscoverManifestInCwd();

void RegisterCodegen(CLI::App& parent, const CliContext& ctx, DeferredAction& pending);

}  // namespace rexglue::cli
