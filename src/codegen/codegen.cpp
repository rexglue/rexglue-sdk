/**
 * @file        codegen/codegen.cpp
 * @brief       The code generation pipeline
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <format>

#include "codegen_categories.h"
#include "pipeline.h"
#include "codegen_writer.h"
#include <rex/logging/log.h>
#include <rex/kernel/title_runtime.h>

#include "tool_runtime.h"

namespace rex::codegen {

CodegenPipeline::~CodegenPipeline() = default;
CodegenPipeline::CodegenPipeline(CodegenPipeline&&) noexcept = default;
CodegenPipeline& CodegenPipeline::operator=(CodegenPipeline&&) noexcept = default;

Result<CodegenPipeline> CodegenPipeline::Create(const std::filesystem::path& config_path,
                                                ToolApp& tools) {
  CodegenPipeline pipeline;

  RecompilerConfig temp_config;
  if (!temp_config.Load(config_path.string())) {
    return Err<CodegenPipeline>(ErrorCategory::kConfig,
                                std::format("Failed to load config: {}", config_path.string()));
  }

  auto config_dir = config_path.parent_path();

  std::filesystem::path xex_path = config_dir / temp_config.file_path;

  if (!std::filesystem::exists(xex_path)) {
    return Err<CodegenPipeline>(ErrorCategory::kIO,
                                std::format("XEX file not found: {}", xex_path.string()));
  }
  xex_path = std::filesystem::canonical(xex_path);

  auto tool = tools.Open(xex_path.parent_path(), xex_path.filename().string());
  if (!tool) {
    return Err<CodegenPipeline>(tool.error());
  }
  pipeline.runtime_ = std::make_unique<ToolRuntime>(std::move(*tool));

  auto ctx_result = CodegenContext::Create(config_path, pipeline.runtime_->GetRuntime());
  if (!ctx_result) {
    return Err<CodegenPipeline>(ctx_result.error());
  }
  pipeline.ctx_ = std::make_unique<CodegenContext>(std::move(*ctx_result));

  return Ok(std::move(pipeline));
}

Result<void> CodegenPipeline::Run(bool force) {
  auto result = RunAnalyze();
  if (!result)
    return result;
  return RunWrite(force);
}

Result<void> CodegenPipeline::RunAnalyze() {
  auto analyze_result = Analyze(*ctx_);
  if (!analyze_result) {
    REXCG_FAIL(analyze_result, "Analysis failed: {}", analyze_result.error().message);
  }
  return Ok();
}

Result<void> CodegenPipeline::RunWrite(bool force) {
  CodegenWriter writer(*ctx_);
  if (!writer.Write(force))
    return Err(ErrorCategory::kValidation, "Code generation failed.");
  return Ok();
}

}  // namespace rex::codegen
