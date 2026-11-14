/**
 * @file        codegen/known_function.h
 * @brief       Builds the known function set and discovers pending functions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <unordered_set>

#include "codegen_context.h"

namespace rex::codegen {

/**
 * Builds the set of non helper function entry points for boundary detection.
 *
 * @param exclude_gap_fill True to leave GAP_FILL authority functions out of the set.
 * @return Entry point addresses.
 */
inline std::unordered_set<u32> BuildKnownFunctions(const FunctionGraph& graph,
                                                   bool exclude_gap_fill = false) {
  std::unordered_set<u32> result;
  for (const auto& [addr, node] : graph.GetFunctions()) {
    auto auth = node->GetAuthority();
    if (auth == FunctionAuthority::kHelper)
      continue;
    if (exclude_gap_fill && auth == FunctionAuthority::kGapFill)
      continue;
    result.insert(addr);
  }
  return result;
}

/**
 * Discovers blocks for all pending functions.
 *
 * @param known_functions Function entry points that bound a function.
 */
size_t DiscoverPendingFunctions(CodegenContext& ctx,
                                const std::unordered_set<u32>& known_functions);

}  // namespace rex::codegen
