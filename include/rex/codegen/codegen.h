/**
 * @file        codegen/codegen.h
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
#include <rex/types.h>

namespace rex {
class Runtime;
}

namespace rex::codegen {

class ToolRuntime;

/**
 * Pipeline orchestrator for code generation.
 *
 * @code
 * auto pipeline = CodegenPipeline::Create(configPath);
 * if (!pipeline) { handle error }
 * auto result = pipeline->Run();
 * @endcode
 */
class CodegenPipeline : public rex::MoveOnly {
 public:
  /** Destroys the pipeline and the runtime and context it owns. */
  ~CodegenPipeline();

  /**
   * Takes ownership of the runtime and context of another pipeline.
   *
   * @param other Pipeline to take from, left empty.
   */
  CodegenPipeline(CodegenPipeline&&) noexcept;
  /**
   * Releases the owned runtime and context, then takes those of another pipeline.
   *
   * @param other Pipeline to take from, left empty.
   * @return This pipeline
   */
  CodegenPipeline& operator=(CodegenPipeline&&) noexcept;

  /**
   * Create pipeline from config file path.
   * Loads XEX, creates Runtime and CodegenContext.
   *
   * @param configPath Path to TOML config file
   * @return Pipeline on success, error on failure
   */
  [[nodiscard]] static Result<CodegenPipeline> Create(const std::filesystem::path& configPath);

  [[nodiscard]] Result<void> Run(bool force = false);
  [[nodiscard]] Result<void> RunAnalyze();
  [[nodiscard]] Result<void> RunWrite(bool force = false);

  CodegenContext& context() { return *ctx_; }
  const CodegenContext& context() const { return *ctx_; }

 private:
  CodegenPipeline() = default;

  std::unique_ptr<ToolRuntime> runtime_;
  std::unique_ptr<CodegenContext> ctx_;
};

}  // namespace rex::codegen
