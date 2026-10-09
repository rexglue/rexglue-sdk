#include <format>

#include <rex/codegen/analyze.h>
#include <rex/codegen/codegen.h>
#include <rex/codegen/codegen_writer.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "tool_runtime.h"

namespace rex::codegen {

CodegenPipeline::~CodegenPipeline() = default;
CodegenPipeline::CodegenPipeline(CodegenPipeline&&) noexcept = default;
CodegenPipeline& CodegenPipeline::operator=(CodegenPipeline&&) noexcept = default;

Result<CodegenPipeline> CodegenPipeline::Create(const std::filesystem::path& configPath) {
  CodegenPipeline pipeline;

  RecompilerConfig tempConfig;
  if (!tempConfig.Load(configPath.string())) {
    return Err<CodegenPipeline>(ErrorCategory::Config,
                                std::format("Failed to load config: {}", configPath.string()));
  }

  auto configDir = configPath.parent_path();

  std::filesystem::path xexPath = configDir / tempConfig.filePath;

  if (!std::filesystem::exists(xexPath)) {
    return Err<CodegenPipeline>(ErrorCategory::IO,
                                std::format("XEX file not found: {}", xexPath.string()));
  }
  xexPath = std::filesystem::canonical(xexPath);

  auto tool = OpenToolRuntime(xexPath.parent_path());
  if (!tool) {
    return Err<CodegenPipeline>(tool.error());
  }
  pipeline.runtime_ = std::make_unique<ToolRuntime>(std::move(*tool));

  auto ctxResult = CodegenContext::Create(configPath, pipeline.runtime_->runtime());
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
    REX_FAIL(codegen, analyzeResult, "Analysis failed: {}", analyzeResult.error().message);
  }
  return Ok();
}

Result<void> CodegenPipeline::RunWrite(bool force) {
  CodegenWriter writer(*ctx_);
  if (!writer.write(force))
    return Err(ErrorCategory::Validation, "Code generation failed.");
  return Ok();
}

}  // namespace rex::codegen
