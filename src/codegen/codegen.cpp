/**
 * @file        codegen/codegen.cpp
 * @brief       Codegen pipeline implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <fmt/format.h>

#include <rex/codegen/analyze.h>
#include <rex/codegen/codegen.h>
#include <rex/codegen/codegen_writer.h>
#include <rex/kernel/init.h>
#include <rex/api.h>
#include <rex/logging.h>
#include <rex/runtime.h>

namespace rex::codegen {

CodegenPipeline::~CodegenPipeline() = default;
CodegenPipeline::CodegenPipeline(CodegenPipeline&&) noexcept = default;
CodegenPipeline& CodegenPipeline::operator=(CodegenPipeline&&) noexcept = default;

Result<CodegenPipeline> CodegenPipeline::Create(const std::filesystem::path& configPath) {
  CodegenPipeline pipeline;

  // Load config to get XEX path
  RecompilerConfig tempConfig;
  if (!tempConfig.Load(configPath.string())) {
    return Err<CodegenPipeline>(ErrorCategory::Config,
                                fmt::format("Failed to load config: {}", configPath.string()));
  }

  auto configDir = configPath.parent_path();

  std::filesystem::path xexPath = configDir / tempConfig.filePath;

  if (!std::filesystem::exists(xexPath)) {
    return Err<CodegenPipeline>(ErrorCategory::IO,
                                fmt::format("XEX file not found: {}", xexPath.string()));
  }
  xexPath = std::filesystem::canonical(xexPath);

  // Create Runtime
  rex::Config runtimeConfig;
  runtimeConfig.paths.game_data_root = xexPath.parent_path();
  pipeline.runtime_ = std::make_unique<Runtime>(runtimeConfig);
  auto status = pipeline.runtime_->Setup(rex::RuntimeConfig{
      .kernel_init = rex::kernel::InitializeKernel,
      .tool_mode = true,
  });
  if (status != X_STATUS_SUCCESS) {
    return Err<CodegenPipeline>(ErrorCategory::IO,
                                fmt::format("Failed to initialize Runtime: {:#x}", status));
  }

  // Create CodegenContext (AnalysisState is populated from binary there)
  auto ctxResult = CodegenContext::Create(configPath, *pipeline.runtime_);
  if (!ctxResult) {
    return Err<CodegenPipeline>(ctxResult.error());
  }
  pipeline.ctx_ = std::make_unique<CodegenContext>(std::move(*ctxResult));

  return Ok(std::move(pipeline));
}

Result<void> CodegenPipeline::Run(bool force) {
  auto result = RunAnalyze();
  if (!result)
    return result;
  return RunWrite(force);
}

Result<void> CodegenPipeline::RunAnalyze() {
  auto analyzeResult = Analyze(*ctx_);
  if (!analyzeResult) {
    REXLOG_ERROR("Analysis failed: {}", analyzeResult.error().message);
    return analyzeResult;
  }
  return Ok();
}

Result<void> CodegenPipeline::RunWrite(bool force) {
  CodegenWriter writer(*ctx_, runtime_.get());
  if (!writer.write(force))
    return Err(ErrorCategory::Validation, "Code generation failed.");
  return Ok();
}

}  // namespace rex::codegen
