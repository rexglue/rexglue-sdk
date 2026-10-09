/**
 * @file        codegen/codegen_writer.h
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

namespace rex::codegen {

/**
 * Generates the source files of a recompiled title from a CodegenContext and writes them to disk.
 */
class CodegenWriter {
 public:
  /**
   * Binds the writer to a context.
   *
   * @param ctx Context to generate from, must outlive the writer
   */
  CodegenWriter(CodegenContext& ctx);

  /**
   * Runs the full output pipeline of validate, generate, flush and sweep.
   *
   * @param force Rewrite outputs whose content is unchanged
   * @return True on success
   */
  bool write(bool force);

  /**
   * Returns the basenames of stale outputs removed by the sweep that follows the flush.
   * Populated only after write() completes, empty otherwise.
   *
   * @return Removed basenames
   */
  const std::vector<std::string>& deletedFiles() const { return deletedFiles_; }

  /**
   * Returns the basenames of files written to disk by FlushPendingWrites() during write().
   * Populated only after write() completes, empty otherwise.
   *
   * @return Written basenames
   */
  const std::vector<std::string>& writtenFiles() const { return writtenFiles_; }

  /**
   * Returns the basenames whose on disk content already matched, so nothing was written.
   *
   * @return Unchanged basenames
   */
  const std::vector<std::string>& unchangedFiles() const { return unchangedFiles_; }

 private:
  CodegenContext& ctx_;

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

  FunctionGraph& graph();
  const FunctionGraph& graph() const;
  const BinaryView& binary() const;
  RecompilerConfig& config();
  const RecompilerConfig& config() const;
  AnalysisState& analysisState();
  const AnalysisState& analysisState() const;
};

}  // namespace rex::codegen
