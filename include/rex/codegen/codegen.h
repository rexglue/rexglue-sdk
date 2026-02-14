/**
 * @file        rex/codegen/codegen.h
 * @brief       Codegen pipeline orchestrator
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <memory>

#include <rex/codegen/codegen_context.h>
#include <rex/result.h>

namespace rex {
class Runtime;
}

namespace rex::codegen {

/**
 * Pipeline orchestrator for code generation.
 *
 * Usage:
 *   auto pipeline = CodegenPipeline::Create(configPath);
 *   if (!pipeline) { handle error }
 *   auto result = pipeline->Run();
 */
class CodegenPipeline {
 public:
  ~CodegenPipeline();

  // Non-copyable, movable
  CodegenPipeline(const CodegenPipeline&) = delete;
  CodegenPipeline& operator=(const CodegenPipeline&) = delete;
  CodegenPipeline(CodegenPipeline&&) noexcept;
  CodegenPipeline& operator=(CodegenPipeline&&) noexcept;

  /**
   * Create pipeline from config file path.
   * Loads XEX, creates Runtime and CodegenContext.
   *
   * @param configPath Path to TOML config file
   * @return Pipeline on success, error on failure
   */
  static Result<CodegenPipeline> Create(const std::filesystem::path& configPath);

  /**
   * Run the full pipeline: Analyze -> Recompile.
   *
   * @param force If true, generate output even with validation errors
   * @return Success or error with description
   */
  Result<void> Run(bool force = false);

  /// Access context for CLI needs (output path, project name, etc.)
  CodegenContext& context() { return *ctx_; }
  const CodegenContext& context() const { return *ctx_; }

 private:
  CodegenPipeline() = default;

  std::unique_ptr<Runtime> runtime_;
  std::unique_ptr<CodegenContext> ctx_;
};

}  // namespace rex::codegen
