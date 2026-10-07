/**
 * @file        rex/codegen/codegen_writer.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <rex/codegen/codegen_context.h>

namespace rex {
class Runtime;
}

namespace rex::codegen {

class CodegenWriter {
 public:
  CodegenWriter(CodegenContext& ctx, Runtime* runtime = nullptr);

  /// Run the full output pipeline: validate, generate, flush, sweep.
  bool write(bool force);

  /**
   * Basenames of stale outputs removed by the sweep that follows the flush.
   * Populated only after write() completes. Empty otherwise.
   */
  const std::vector<std::string>& deletedFiles() const { return deletedFiles_; }

  /**
   * Basenames of files written to disk during write() (via FlushPendingWrites).
   * Populated only after write() completes. Empty otherwise.
   */
  const std::vector<std::string>& writtenFiles() const { return writtenFiles_; }

  /// Basenames whose on-disk content already matched, so nothing was written.
  const std::vector<std::string>& unchangedFiles() const { return unchangedFiles_; }

 private:
  CodegenContext& ctx_;
  Runtime* runtime_;

  std::string out;
  std::vector<std::pair<std::string, std::string>> pendingWrites;
  std::vector<std::string> deletedFiles_;
  std::vector<std::string> writtenFiles_;
  std::vector<std::string> unchangedFiles_;

  template <class... Args>
  void print(std::format_string<Args...> fmt, Args&&... args) {
    std::vformat_to(std::back_inserter(out), fmt.get(), std::make_format_args(args...));
  }

  template <class... Args>
  void println(std::format_string<Args...> fmt, Args&&... args) {
    std::vformat_to(std::back_inserter(out), fmt.get(), std::make_format_args(args...));
    out += '\n';
  }

  void SaveCurrentOutData(std::string_view name);
  bool FlushPendingWrites();

  // Convenience accessors
  FunctionGraph& graph();
  const FunctionGraph& graph() const;
  const BinaryView& binary() const;
  RecompilerConfig& config();
  const RecompilerConfig& config() const;
  AnalysisState& analysisState();
  const AnalysisState& analysisState() const;
};

}  // namespace rex::codegen
