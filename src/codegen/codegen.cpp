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
#include <rex/logging.h>
#include <rex/runtime.h>

#include "codegen_logging.h"

namespace rex::codegen {

namespace {

/// Override the auto-generated `sub_XXXXXXXX` names on FunctionNodes
/// using `cfg.functionNames` (loaded by RecompilerConfig::LoadMappings
/// from the user's `mapping_file_path`). Runs after Analyze so the
/// graph is fully populated, and before code emission so the new
/// names propagate everywhere `FunctionNode::name()` is consumed.
///
/// Skip rules: import functions keep their `__imp__` names (set by
/// the import resolver) and helper functions keep their
/// `__restgprlr_*` / `__savevmx_*` names (the runtime ABI depends on
/// these). Mapping addresses that don't correspond to a FunctionNode
/// entry are split into "intra-function" (address is a label inside a
/// parent function -- expected under rexglue's chunks-as-labels
/// model) and "missing" (address not in the binary at all -- usually
/// a sign the mapping was built from a sibling-game PDB). Both
/// classes are summarised at INFO; individual misses are TRACE so the
/// log stays clean for the typical 50k-entry mapping.
void ApplyMappingNames(CodegenContext& ctx) {
  const auto& cfg = ctx.Config();
  if (cfg.functionNames.empty())
    return;

  size_t renamed = 0;
  size_t skippedImport = 0;
  size_t skippedHelper = 0;
  size_t intraFunction = 0;
  size_t missing = 0;

  for (const auto& [address, name] : cfg.functionNames) {
    auto it = ctx.graph.functions().find(address);
    if (it == ctx.graph.functions().end()) {
      // Not a function entry. Could be a label inside a parent
      // function (rexglue's chunks-as-labels model exposes these as
      // labels rather than separate FunctionNodes), or could be an
      // address that isn't in this binary at all.
      if (ctx.graph.getFunctionContaining(address) != nullptr) {
        ++intraFunction;
        REXCODEGEN_TRACE("[mapping] 0x{:08X} ({}) is intra-function; skipping rename", address,
                         name);
      } else {
        ++missing;
        REXCODEGEN_TRACE("[mapping] 0x{:08X} ({}) not in binary; skipping rename", address, name);
      }
      continue;
    }

    const auto& node = *it->second;
    if (node.isImport()) {
      ++skippedImport;
      continue;
    }
    if (node.isHelper()) {
      ++skippedHelper;
      continue;
    }

    ctx.graph.setFunctionName(address, name);
    ++renamed;
  }

  REXCODEGEN_INFO(
      "[mapping] renamed {} function symbol(s) from mapping TOML "
      "({} intra-function skipped, {} missing, {} import-preserved, {} helper-preserved)",
      renamed, intraFunction, missing, skippedImport, skippedHelper);
}

}  // namespace

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
    return Err<CodegenPipeline>(ErrorCategory::IO,
                                fmt::format("XEX file not found: {}", xexPath.string()));
  }
  xexPath = std::filesystem::canonical(xexPath);

  // Create Runtime
  auto xexDir = xexPath.parent_path();
  pipeline.runtime_ = std::make_unique<Runtime>(xexDir.string());
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
  // Phase 1: Analyze (builds and validates function graph)
  auto analyzeResult = Analyze(*ctx_);
  if (!analyzeResult) {
    REXLOG_ERROR("Analysis failed: {}", analyzeResult.error().message);
    if (!force) {
      return analyzeResult;
    }
    REXLOG_WARN("Analysis errors ignored due to --force; output may be incomplete");
  }

  // Phase 1.5: Override auto-generated sub_XXXXXXXX names with the
  // user's mapping TOML (no-op when no mapping is configured).
  ApplyMappingNames(*ctx_);

  // Phase 2: Generate C++ output
  CodegenWriter writer(*ctx_, runtime_.get());
  if (!writer.write(force))
    return Err(ErrorCategory::Validation, "Code generation failed.");
  return Ok();
}

}  // namespace rex::codegen
