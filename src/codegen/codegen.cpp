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

#include <rex/codegen/codegen.h>
#include <rex/codegen/analyze.h>
#include <rex/codegen/recompile.h>
#include <rex/runtime.h>
#include <rex/logging.h>
#include <fmt/format.h>

namespace rex::codegen {

CodegenPipeline::~CodegenPipeline() = default;
CodegenPipeline::CodegenPipeline(CodegenPipeline&&) noexcept = default;
CodegenPipeline& CodegenPipeline::operator=(CodegenPipeline&&) noexcept = default;

Result<CodegenPipeline> CodegenPipeline::Create(const std::filesystem::path& configPath) {
    CodegenPipeline pipeline;

    // Load config to get XEX path
    RecompilerConfig tempConfig;
    tempConfig.Load(configPath.string());

    auto configDir = configPath.parent_path();

    // Determine XEX path
    std::filesystem::path xexPath;
    if (!tempConfig.patchedFilePath.empty()) {
        xexPath = configDir / tempConfig.patchedFilePath;
        if (!std::filesystem::exists(xexPath)) {
            xexPath.clear();
        }
    }
    if (xexPath.empty()) {
        xexPath = configDir / tempConfig.filePath;
    }

    if (!std::filesystem::exists(xexPath)) {
        return Err<CodegenPipeline>(ErrorCategory::IO, fmt::format("XEX file not found: {}", xexPath.string()));
    }
    xexPath = std::filesystem::canonical(xexPath);

    // Create Runtime
    auto xexDir = xexPath.parent_path();
    pipeline.runtime_ = std::make_unique<Runtime>(xexDir.string());
    auto status = pipeline.runtime_->Setup(true);
    if (status != X_STATUS_SUCCESS) {
        return Err<CodegenPipeline>(ErrorCategory::IO, fmt::format("Failed to initialize Runtime: {:#x}", status));
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
    // Phase 1: Analyze (builds and validates function graph)
    auto analyzeResult = Analyze(*ctx_);
    if (!analyzeResult) {
        REXLOG_ERROR("Analysis failed: {}", analyzeResult.error().message);
        return analyzeResult;
    }

    // Phase 2: Recompile (generates C++ code)
    return Recompile(*ctx_, force);
}

} // namespace rex::codegen
