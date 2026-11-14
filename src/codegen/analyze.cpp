/**
 * @file        codegen/analyze.cpp
 * @brief       Runs the analysis phases in order
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "decoded_binary.h"

#include <algorithm>
#include <map>

#include "analysis_error_log.h"
#include "pipeline.h"
#include <rex/logging/log_category.h>
#include "codegen_categories.h"

namespace rex::codegen {

[[nodiscard]] Result<void> Analyze(CodegenContext& ctx, IProgressReporter* reporter) {
  REXCG_TRACE("Analyze: starting analysis...");

  ctx.InitDecoded();
  REXCG_TRACE("Analyze: decoded {} instructions across {} code regions",
              ctx.GetDecoded().GetInstructionCount(), ctx.GetDecoded().GetCodeRegions().size());

  if (reporter)
    reporter->ModulePhaseStarted("Register");
  auto reg_result = detail::RegisterFunctions(ctx, reporter);
  if (!reg_result) {
    return reg_result;
  }

  if (reporter)
    reporter->ModulePhaseStarted("Scan");
  auto scan_result = detail::ScanRegions(ctx, reporter);
  if (!scan_result) {
    return scan_result;
  }

  if (reporter)
    reporter->ModulePhaseStarted("Discover");
  auto discover_result = detail::DiscoverFunctions(ctx, reporter);
  if (!discover_result) {
    return discover_result;
  }

  if (reporter)
    reporter->ModulePhaseStarted("GapFill");
  auto gap_fill_result = detail::GapFill(ctx, reporter);
  if (!gap_fill_result) {
    return gap_fill_result;
  }

  if (reporter)
    reporter->ModulePhaseStarted("Merge");
  auto merge_result = detail::MergeFunctions(ctx, reporter);
  if (!merge_result) {
    return merge_result;
  }

  if (reporter)
    reporter->ModulePhaseStarted("Validate");
  auto validate_result = detail::ValidateGraph(ctx);
  if (!validate_result) {
    return validate_result;
  }

  REXCG_TRACE("Analyze: complete - {} functions ready for code generation",
              ctx.graph.GetFunctionCount());

  return Ok();
}

const char* AnalysisErrorLog::CategoryName(Category cat) {
  switch (cat) {
    case Category::kUnresolvedCall:
      return "UnresolvedCall";
    case Category::kMillicode:
      return "Millicode";
    default:
      return "Unknown";
  }
}

void AnalysisErrorLog::Add(Category cat, u32 addr, const std::string& msg) {
  Add(cat, addr, 0, msg);
}

void AnalysisErrorLog::Add(Category cat, u32 addr, u32 secondary, const std::string& msg) {
  entries_.push_back({cat, addr, secondary, msg});
}

size_t AnalysisErrorLog::Count(Category cat) const {
  return std::count_if(entries_.begin(), entries_.end(),
                       [cat](const Entry& e) { return e.category == cat; });
}

void AnalysisErrorLog::PrintReport() const {
  if (entries_.empty()) {
    return;
  }

  REXCG_ERROR("=== ANALYSIS ERRORS ===");

  std::map<Category, std::vector<const Entry*>> by_category;
  for (const auto& entry : entries_) {
    by_category[entry.category].push_back(&entry);
  }

  for (const auto& [cat, entries] : by_category) {
    REXCG_ERROR("{} ({}):", CategoryName(cat), entries.size());

    for (const auto* entry : entries) {
      if (entry->secondary_address != 0) {
        REXCG_ERROR("  0x{:08X} from 0x{:08X}: {}", entry->address, entry->secondary_address,
                    entry->message);
      } else {
        REXCG_ERROR("  0x{:08X}: {}", entry->address, entry->message);
      }
    }
  }

  REXCG_ERROR("Total: {} errors", entries_.size());
}

}  // namespace rex::codegen
