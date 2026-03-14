/**
 * @file        codegen/phase_helpers.h
 * @brief       Shared helpers for analysis phase files
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <unordered_set>

#include <rex/codegen/codegen_context.h>

namespace rex::codegen {

/// Build set of non-helper function entry points for boundary detection.
/// @param graph The function graph to scan
/// @param excludeGapFill If true, also exclude GAP_FILL authority functions
inline std::unordered_set<uint32_t> buildKnownFunctions(const FunctionGraph& graph,
                                                        bool excludeGapFill = false) {
  std::unordered_set<uint32_t> result;
  for (const auto& [addr, node] : graph.functions()) {
    auto auth = node->authority();
    if (auth == FunctionAuthority::HELPER)
      continue;
    if (excludeGapFill && auth == FunctionAuthority::GAP_FILL)
      continue;
    result.insert(addr);
  }
  return result;
}

/// Discover blocks for all pending functions.
size_t discoverPendingFunctions(CodegenContext& ctx,
                                const std::unordered_set<uint32_t>& knownFunctions);

}  // namespace rex::codegen
