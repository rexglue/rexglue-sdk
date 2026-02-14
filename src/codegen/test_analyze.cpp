/**
 * @file        codegen/test_analyze.cpp
 * @brief       Test binary analysis - populates FunctionGraph from map symbols
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "ppc/instruction.h"

#include <rex/codegen/codegen_context.h>
#include <rex/codegen/test_analyze.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/types.h>

using rex::codegen::ppc::decode_instruction;
using rex::memory::load_and_swap;
#include <algorithm>
#include <vector>

#include <fmt/format.h>

namespace rex::codegen {

void AnalyzeTestBinary(CodegenContext& ctx, std::string_view testName,
                       const std::map<size_t, std::string>& symbols, uint32_t baseAddress,
                       const uint8_t* data, size_t dataSize) {
  // Extract only test_ prefixed symbols as function entry points
  std::vector<std::pair<size_t, std::string>> testFunctions;
  for (const auto& [addr, name] : symbols) {
    if (name.starts_with("test_")) {
      testFunctions.push_back({addr, name});
    }
  }

  // Sort by address
  std::sort(testFunctions.begin(), testFunctions.end());

  // First pass: add all functions to the graph
  for (size_t i = 0; i < testFunctions.size(); i++) {
    uint32_t fnAddr = static_cast<uint32_t>(testFunctions[i].first);

    // Size extends to next test_ function or end of binary
    uint32_t nextAddr = static_cast<uint32_t>(baseAddress + dataSize);
    if (i + 1 < testFunctions.size()) {
      nextAddr = static_cast<uint32_t>(testFunctions[i + 1].first);
    }
    uint32_t fnSize = nextAddr - fnAddr;

    auto* node = ctx.graph.addFunction(fnAddr, fnSize, FunctionAuthority::DISCOVERED, true);

    if (node) {
      ctx.graph.addBlockToFunction(fnAddr, {fnAddr, fnSize});
      ctx.graph.setFunctionName(fnAddr, fmt::format("{}_{:X}", testName, fnAddr));
    }
  }

  // Second pass: scan for bl instructions and register call edges
  for (size_t i = 0; i < testFunctions.size(); i++) {
    uint32_t fnAddr = static_cast<uint32_t>(testFunctions[i].first);
    uint32_t nextAddr = static_cast<uint32_t>(baseAddress + dataSize);
    if (i + 1 < testFunctions.size()) {
      nextAddr = static_cast<uint32_t>(testFunctions[i + 1].first);
    }
    uint32_t fnSize = nextAddr - fnAddr;

    // Scan each instruction in this function
    for (uint32_t offset = 0; offset < fnSize; offset += 4) {
      uint32_t pc = fnAddr + offset;
      uint32_t raw = load_and_swap<uint32_t>(data + (fnAddr - baseAddress) + offset);
      auto decoded = decode_instruction(pc, raw);

      // Check for bl (branch with link = function call)
      if (decoded.is_call() && decoded.branch_target.has_value()) {
        ctx.graph.addUnresolvedJumpToFunction(fnAddr, pc, decoded.branch_target.value(), true,
                                              false);
      }
    }
  }
}

}  // namespace rex::codegen
