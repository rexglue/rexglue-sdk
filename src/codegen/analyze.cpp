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
#include <rex/logging.h>

#include "codegen_logging.h"

namespace rex::codegen {

Result<void> Analyze(CodegenContext& ctx) {
  REXCODEGEN_INFO("Analyze: starting analysis...");

  ctx.initDecoded();
  REXCODEGEN_INFO("Analyze: decoded {} instructions across {} code regions",
                  ctx.decoded().instructionCount(), ctx.decoded().codeRegions().size());

  // 1. Register entry points (imports, helpers, config, pdata)
  auto regResult = phases::Register(ctx);
  if (!regResult) {
    return regResult;
  }

  // 2. Scan binary into code/data regions
  auto scanResult = phases::Scan(ctx);
  if (!scanResult) {
    return scanResult;
  }

  // 3. Discover function blocks iteratively (includes vtable scan)
  auto discoverResult = phases::Discover(ctx);
  if (!discoverResult) {
    return discoverResult;
  }

  // 3.5. Function pointer scan: find lis/addi pairs loading code addresses
  // TODO(tomc): disabled for now, causes too many false positives
  // functionPointerScan(ctx);

  // 4. Gap fill uncovered regions + discover blocks for gap-filled functions + cleanup
  auto gapFillResult = phases::GapFill(ctx);
  if (!gapFillResult) {
    return gapFillResult;
  }

  // 5. Merge: resolve jumps and seal functions
  auto mergeResult = phases::Merge(ctx);
  if (!mergeResult) {
    return mergeResult;
  }

  // 6. Validate
  auto validateResult = phases::Validate(ctx);
  if (!validateResult) {
    return validateResult;
  }

  REXCODEGEN_INFO("Analyze: complete - {} functions ready for code generation",
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
