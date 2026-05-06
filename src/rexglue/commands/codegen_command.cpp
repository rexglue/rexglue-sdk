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

#include <filesystem>

#include <fmt/format.h>

#include <rex/codegen/codegen.h>
#include <rex/logging.h>

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
  // --mapping <path> wins over the config TOML's mapping_file_path.
  // Resolve against cwd (CLI semantic, not config-relative) and call
  // LoadMappings() again so the prior contents -- if any -- are
  // replaced rather than merged.
  if (!ctx.mappingPath.empty()) {
    std::filesystem::path resolved(ctx.mappingPath);
    if (resolved.is_relative())
      resolved = std::filesystem::absolute(resolved);
    auto& cfg = pipeline->context().Config();
    cfg.mappingFilePath = resolved.string();
    if (!cfg.LoadMappings()) {
      return Err(rex::ErrorCategory::Config,
                 fmt::format("Failed to load mapping TOML: {}", resolved.string()));
    }
  }

  return pipeline->Run(ctx.force);
}

}  // namespace rexglue::cli
