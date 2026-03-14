/**
 * @file        rex/codegen/codegen_writer.h
 * @brief       Consolidated codegen output writer
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <fmt/core.h>

#include <rex/codegen/codegen_context.h>

namespace rex {
class Runtime;
}

namespace rex::codegen {

class CodegenWriter {
 public:
  CodegenWriter(CodegenContext& ctx, Runtime* runtime = nullptr);

  /// Run the full output pipeline: validate, clean old files, generate, flush.
  bool write(bool force);

 private:
  CodegenContext& ctx_;
  Runtime* runtime_;

  std::string out;
  size_t cppFileIndex = 0;
  std::vector<std::pair<std::string, std::string>> pendingWrites;

  template <class... Args>
  void print(fmt::format_string<Args...> fmt, Args&&... args) {
    fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
  }

  template <class... Args>
  void println(fmt::format_string<Args...> fmt, Args&&... args) {
    fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
    out += '\n';
  }

  void SaveCurrentOutData(std::string_view name = {});
  void FlushPendingWrites();

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
