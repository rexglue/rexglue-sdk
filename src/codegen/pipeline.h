/**
 * @file        codegen/pipeline.h
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
#include <string>

#include "codegen_context.h"
#include "progress_reporter.h"
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex {
namespace kernel {
class TitleRuntime;
}  // namespace kernel
}  // namespace rex

namespace rex::codegen {

class ToolApp;
class ToolRuntime;

class CodegenPipeline : public rex::MoveOnly {
 public:
  ~CodegenPipeline();

  CodegenPipeline(CodegenPipeline&&) noexcept;
  CodegenPipeline& operator=(CodegenPipeline&&) noexcept;

  /**
   * Creates the pipeline from a config file and loads the XEX.
   *
   * @param tools App that opens the tool runtime.
   * @return The pipeline, or the failure.
   */
  [[nodiscard]] static Result<CodegenPipeline> Create(const std::filesystem::path& config_path,
                                                      ToolApp& tools);

  /**
   * Runs analysis, then writes the generated code.
   *
   * @param force Write even when the analysis collected errors.
   */
  [[nodiscard]] Result<void> Run(bool force = false);
  [[nodiscard]] Result<void> RunAnalyze();
  /**
   * Writes the generated code.
   *
   * @param force Write even when the analysis collected errors.
   */
  [[nodiscard]] Result<void> RunWrite(bool force = false);

  CodegenContext& GetContext() { return *ctx_; }
  const CodegenContext& GetContext() const { return *ctx_; }

 private:
  CodegenPipeline() = default;

  std::unique_ptr<ToolRuntime> runtime_;
  std::unique_ptr<CodegenContext> ctx_;
};

/**
 * Builds and validates the FunctionGraph. The phases run in this order.
 * - Register imports, helpers, PDATA functions and config functions
 * - Scan code regions (null boundaries) and data regions
 * - Discover function blocks, calls and jump tables, iterating to a fixed point
 * - VTable discovers functions through RTTI vtables
 * - GapFill claims orphaned code regions
 * - Merge resolves all jumps and seals functions
 * - Validate verifies that all calls resolve
 *
 * @param ctx CodegenContext with binary and config loaded.
 * @param reporter Optional progress sink, may be null.
 */
[[nodiscard]] Result<void> Analyze(CodegenContext& ctx, IProgressReporter* reporter = nullptr);

}  // namespace rex::codegen

namespace rex::codegen::detail {

/**
 * Registers PDATA, CONFIG, helpers and imports into FunctionGraph.
 *
 * @param ctx CodegenContext with binary and config loaded
 * @param reporter Optional progress sink, may be null
 */
[[nodiscard]] Result<void> RegisterFunctions(CodegenContext& ctx,
                                             IProgressReporter* reporter = nullptr);

/**
 * Segments the executable sections into code and data regions.
 *
 * @param ctx CodegenContext with binary and config loaded
 * @param reporter Optional progress sink, may be null
 */
[[nodiscard]] Result<void> ScanRegions(CodegenContext& ctx, IProgressReporter* reporter = nullptr);

/**
 * Discovers function blocks, calls and jump tables.
 *
 * @param ctx CodegenContext with binary and config loaded
 * @param reporter Optional progress sink, may be null
 */
[[nodiscard]] Result<void> DiscoverFunctions(CodegenContext& ctx,
                                             IProgressReporter* reporter = nullptr);

/**
 * Finds uncovered code regions and registers them as functions. Runs before Merge.
 *
 * @param ctx CodegenContext with binary and config loaded
 * @param reporter Optional progress sink, may be null
 */
[[nodiscard]] Result<void> GapFill(CodegenContext& ctx, IProgressReporter* reporter = nullptr);

/**
 * Resolves every jump to a label, call or import, marks funclets that share registers, and
 * seals the functions that are ready. Runs after GapFill.
 *
 * @param ctx CodegenContext with binary and config loaded
 * @param reporter Optional progress sink, may be null
 */
[[nodiscard]] Result<void> MergeFunctions(CodegenContext& ctx,
                                          IProgressReporter* reporter = nullptr);

/**
 * Validates that all call targets resolve.
 *
 * @param ctx CodegenContext with binary and config loaded
 */
[[nodiscard]] Result<void> ValidateGraph(CodegenContext& ctx);

}  // namespace rex::codegen::detail
