#include "decoded_binary.h"

#include <algorithm>
#include <map>

#include <rex/codegen/analysis_errors.h>
#include <rex/codegen/analyze.h>
#include <rex/codegen/phases.h>
#include <rex/logging/log.h>

namespace rex::codegen {

[[nodiscard]] Result<void> Analyze(CodegenContext& ctx, ProgressReporter* reporter) {
  REXLOG_TRACE(codegen, "Analyze: starting analysis...");

  ctx.InitDecoded();
  REXLOG_TRACE(codegen, "Analyze: decoded {} instructions across {} code regions",
               ctx.GetDecoded().GetInstructionCount(), ctx.GetDecoded().GetCodeRegions().size());

  if (reporter)
    reporter->PhaseChanged("Register");
  auto reg_result = detail::Register(ctx, reporter);
  if (!reg_result) {
    return reg_result;
  }

  if (reporter)
    reporter->PhaseChanged("Scan");
  auto scan_result = detail::Scan(ctx, reporter);
  if (!scan_result) {
    return scan_result;
  }

  if (reporter)
    reporter->PhaseChanged("Discover");
  auto discover_result = detail::Discover(ctx, reporter);
  if (!discover_result) {
    return discover_result;
  }

  if (reporter)
    reporter->PhaseChanged("GapFill");
  auto gap_fill_result = detail::GapFill(ctx, reporter);
  if (!gap_fill_result) {
    return gap_fill_result;
  }

  if (reporter)
    reporter->PhaseChanged("Merge");
  auto merge_result = detail::Merge(ctx, reporter);
  if (!merge_result) {
    return merge_result;
  }

  if (reporter)
    reporter->PhaseChanged("Validate");
  auto validate_result = detail::Validate(ctx, reporter);
  if (!validate_result) {
    return validate_result;
  }

  REXLOG_TRACE(codegen, "Analyze: complete - {} functions ready for code generation",
               ctx.graph.GetFunctionCount());

  return Ok();
}

const char* AnalysisErrors::CategoryName(Category cat) {
  switch (cat) {
    case Category::kUnresolvedCall:
      return "UnresolvedCall";
    case Category::kMissingJumpTable:
      return "MissingJumpTable";
    case Category::kJumpTargetOutOfBounds:
      return "JumpTargetOutOfBounds";
    case Category::kDiscontinuousFunction:
      return "DiscontinuousFunction";
    case Category::kUnimplementedInsn:
      return "UnimplementedInsn";
    case Category::kMillicode:
      return "Millicode";
    default:
      return "Unknown";
  }
}

void AnalysisErrors::Add(Category cat, u32 addr, const std::string& msg) {
  Add(cat, addr, 0, msg);
}

void AnalysisErrors::Add(Category cat, u32 addr, u32 secondary, const std::string& msg) {
  entries_.push_back({cat, addr, secondary, msg});
}

size_t AnalysisErrors::Count(Category cat) const {
  return std::count_if(entries_.begin(), entries_.end(),
                       [cat](const Entry& e) { return e.category == cat; });
}

void AnalysisErrors::PrintReport() const {
  if (entries_.empty()) {
    return;
  }

  REXLOG_ERROR(codegen, "=== ANALYSIS ERRORS ===");

  std::map<Category, std::vector<const Entry*>> by_category;
  for (const auto& entry : entries_) {
    by_category[entry.category].push_back(&entry);
  }

  for (const auto& [cat, entries] : by_category) {
    REXLOG_ERROR(codegen, "{} ({}):", CategoryName(cat), entries.size());

    for (const auto* entry : entries) {
      if (entry->secondary_address != 0) {
        REXLOG_ERROR(codegen, "  0x{:08X} from 0x{:08X}: {}", entry->address,
                     entry->secondary_address, entry->message);
      } else {
        REXLOG_ERROR(codegen, "  0x{:08X}: {}", entry->address, entry->message);
      }
    }
  }

  REXLOG_ERROR(codegen, "Total: {} errors", entries_.size());
}

}  // namespace rex::codegen
