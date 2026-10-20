/**
 * @file        codegen/analyze.cpp
 * @brief       Analysis pipeline orchestrator
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

#include <rex/codegen/analysis_errors.h>
#include <rex/codegen/analyze.h>
#include <rex/codegen/phases.h>
#include <string>
#include <unordered_map>

#include <rex/logging.h>

#include "codegen_logging.h"

namespace rex::codegen {

Result<void> Analyze(CodegenContext& ctx, ProgressReporter* reporter) {
  REXCODEGEN_TRACE("Analyze: starting analysis...");

  ctx.initDecoded();
  REXCODEGEN_TRACE("Analyze: decoded {} instructions across {} code regions",
                   ctx.decoded().instructionCount(), ctx.decoded().codeRegions().size());

  // 1. Register entry points (imports, helpers, config, pdata)
  if (reporter)
    reporter->phaseChanged("Register");
  auto regResult = phases::Register(ctx, reporter);
  if (!regResult) {
    return regResult;
  }

  // 2. Scan binary into code/data regions
  if (reporter)
    reporter->phaseChanged("Scan");
  auto scanResult = phases::Scan(ctx, reporter);
  if (!scanResult) {
    return scanResult;
  }

  // 3. Discover function blocks iteratively (includes vtable scan)
  if (reporter)
    reporter->phaseChanged("Discover");
  auto discoverResult = phases::Discover(ctx, reporter);
  if (!discoverResult) {
    return discoverResult;
  }

  // 3.5. Function pointer scan: find lis/addi pairs loading code addresses
  // TODO(tomc): disabled for now, causes too many false positives
  // functionPointerScan(ctx);

  // 4. Gap fill uncovered regions + discover blocks for gap-filled functions + cleanup
  if (reporter)
    reporter->phaseChanged("GapFill");
  auto gapFillResult = phases::GapFill(ctx, reporter);
  if (!gapFillResult) {
    return gapFillResult;
  }

  // 5. Merge: resolve jumps and seal functions
  if (reporter)
    reporter->phaseChanged("Merge");
  auto mergeResult = phases::Merge(ctx, reporter);
  if (!mergeResult) {
    return mergeResult;
  }

  // 6. Validate
  if (reporter)
    reporter->phaseChanged("Validate");
  auto validateResult = phases::Validate(ctx, reporter);
  if (!validateResult) {
    return validateResult;
  }

  // 7. Readable names ([names]): only for functions the analysis found.
  {
    size_t named = 0, missing = 0;
    std::unordered_map<std::string, uint32_t> used;
    for (const auto& [address, name] : ctx.Config().functionNames) {
      if (!ctx.graph.getFunction(address)) {
        ++missing;
        REXCODEGEN_DEBUG("[names] 0x{:08X} ({}) is not a function start; skipped", address, name);
        continue;
      }
      auto [it, inserted] = used.emplace(name, address);
      if (!inserted) {
        REXCODEGEN_ERROR("[names] {} is given to both 0x{:08X} and 0x{:08X}", name, it->second,
                         address);
        return Err(ErrorCategory::Config, "duplicate name in [names]: " + name);
      }
      ctx.graph.setFunctionName(address, name);
      ++named;
    }
    if (named || missing) {
      REXCODEGEN_INFO("[names] {} functions named, {} skipped (not function starts)", named,
                      missing);
    }
  }

  REXCODEGEN_TRACE("Analyze: complete - {} functions ready for code generation",
                   ctx.graph.functionCount());

  return Ok();
}

//=============================================================================
// AnalysisErrors implementation
//=============================================================================

const char* AnalysisErrors::CategoryName(Category cat) {
  switch (cat) {
    case Category::UnresolvedCall:
      return "UnresolvedCall";
    case Category::MissingJumpTable:
      return "MissingJumpTable";
    case Category::JumpTargetOutOfBounds:
      return "JumpTargetOutOfBounds";
    case Category::DiscontinuousFunction:
      return "DiscontinuousFunction";
    case Category::UnimplementedInsn:
      return "UnimplementedInsn";
    default:
      return "Unknown";
  }
}

void AnalysisErrors::Add(Category cat, uint32_t addr, const std::string& msg) {
  Add(cat, addr, 0, msg);
}

void AnalysisErrors::Add(Category cat, uint32_t addr, uint32_t secondary, const std::string& msg) {
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

  REXCODEGEN_ERROR("=== ANALYSIS ERRORS ===");

  // Group by category
  std::map<Category, std::vector<const Entry*>> byCategory;
  for (const auto& entry : entries_) {
    byCategory[entry.category].push_back(&entry);
  }

  for (const auto& [cat, entries] : byCategory) {
    REXCODEGEN_ERROR("{} ({}):", CategoryName(cat), entries.size());

    for (const auto* entry : entries) {
      if (entry->secondaryAddress != 0) {
        REXCODEGEN_ERROR("  0x{:08X} from 0x{:08X}: {}", entry->address, entry->secondaryAddress,
                         entry->message);
      } else {
        REXCODEGEN_ERROR("  0x{:08X}: {}", entry->address, entry->message);
      }
    }
  }

  REXCODEGEN_ERROR("Total: {} errors", entries_.size());
}

}  // namespace rex::codegen
