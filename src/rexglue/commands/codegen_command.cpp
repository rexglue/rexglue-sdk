/**
 * @file        rexglue/commands/codegen_command.cpp
 * @brief       Code generation command implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "codegen_command.h"
#include <rex/logging.h>
#include <rex/codegen/codegen.h>

#include <fmt/format.h>
#include <filesystem>

namespace rexglue::cli {

Result<void> CodegenFromConfig(const std::string& config_path, const CliContext& ctx) {
    REXLOG_INFO("Generating code with config: {}", config_path);

    auto pipeline = rex::codegen::CodegenPipeline::Create(config_path);
    if (!pipeline) {
        return Err<void>(pipeline.error());
    }

    // Apply CLI overrides to config
    if (ctx.enableExceptionHandlers) {
        pipeline->context().Config().generateExceptionHandlers = true;
        REXLOG_INFO("Exception handler generation enabled");
    }

    return pipeline->Run(ctx.force);
}

} // namespace rexglue::cli
