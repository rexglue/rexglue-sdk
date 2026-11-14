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

#include "codegen_context.h"

namespace rex::codegen {

/**
 * Generates the source files of a recompiled title from a CodegenContext and writes them to disk.
 */
class CodegenWriter {
 public:
  /**
   * @param ctx Context to generate from, must outlive the writer
   */
  CodegenWriter(CodegenContext& ctx);

  /**
   * Runs the output pipeline of validate, generate, flush and sweep.
   *
   * @param force Rewrite outputs whose content is unchanged
   */
  bool Write(bool force);

  /**
   * Returns the basenames of stale outputs removed by the sweep that follows the flush.
   * Populated only after Write() completes, empty otherwise.
   */
  const std::vector<std::string>& GetDeletedFiles() const { return deleted_files_; }

  /**
   * Returns the basenames of files written to disk by FlushPendingWrites() during Write().
   * Populated only after Write() completes, empty otherwise.
   */
  const std::vector<std::string>& GetWrittenFiles() const { return written_files_; }

  /** Returns the basenames whose on disk content matched, so nothing was written. */
  const std::vector<std::string>& GetUnchangedFiles() const { return unchanged_files_; }

 private:
  CodegenContext& ctx_;

  std::string out_;
  std::vector<std::pair<std::string, std::string>> pending_writes_;
  std::vector<std::string> deleted_files_;
  std::vector<std::string> written_files_;
  std::vector<std::string> unchanged_files_;

  template <class... Args>
  void Print(std::format_string<Args...> fmt, Args&&... args) {
    std::vformat_to(std::back_inserter(out_), fmt.get(), std::make_format_args(args...));
  }

  template <class... Args>
  void Println(std::format_string<Args...> fmt, Args&&... args) {
    std::vformat_to(std::back_inserter(out_), fmt.get(), std::make_format_args(args...));
    out_ += '\n';
  }

  void QueueOutput(std::string_view name);
  bool FlushPendingWrites();

  FunctionGraph& GetGraph();
  const FunctionGraph& GetGraph() const;
  const BinaryView& GetBinary() const;
  RecompilerConfig& GetConfig();
  const RecompilerConfig& GetConfig() const;
  AnalysisState& GetAnalysisState();
  const AnalysisState& GetAnalysisState() const;
};

}  // namespace rex::codegen
