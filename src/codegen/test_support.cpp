/**
 * @file        codegen/test_support.cpp
 * @brief       Test support utilities
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/codegen/test_support.h>

#include <algorithm>
#include <vector>

#include <fmt/format.h>

#include "ppc/instruction.h"

#include <rex/codegen/codegen_context.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/binary_types.h>
#include <rex/types.h>

using rex::codegen::ppc::decode_instruction;
using rex::memory::load_and_swap;

namespace rex::codegen {

// --- TestModule ---

TestModule::TestModule() : Module(nullptr) {}

void TestModule::Load(uint32_t base_address, const uint8_t* data, size_t size) {
  base_address_ = base_address;
  size_ = static_cast<uint32_t>(size);

  // Populate binary section for FunctionScanner to read instructions
  binary_sections_.clear();
  binary_sections_.push_back(runtime::BinarySection{
      ".text", base_address, static_cast<uint32_t>(size), data,
      true,  // executable
      false  // writable
  });
}

bool TestModule::ContainsAddress(uint32_t address) {
  return address >= base_address_ && address < base_address_ + size_;
}

std::unique_ptr<runtime::Function> TestModule::CreateFunction(uint32_t /*address*/) {
  // NOTE(tomc) - this is done else where in the codegen tests,
  // and we don't need actual Function objects for the test cases, so just return null.
  return nullptr;
}

// --- AnalyzeTestBinary ---

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

  // First pass: add all functions and transition through state machine
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
      // kRegistered -> kDiscovered -> kSealed
      node->discover({{fnAddr, fnSize}}, {}, {});
      ctx.graph.setFunctionName(fnAddr, fmt::format("{}_{:X}", testName, fnAddr));
      node->seal();
    }
  }

  // Second pass: scan for bl instructions and register resolved call edges
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
        uint32_t target = decoded.branch_target.value();
        auto* targetNode = ctx.graph.getFunction(target);
        if (targetNode) {
          ctx.graph.addCallToFunction(fnAddr, pc, CallTarget::function(targetNode));
        }
      }
    }
  }
}

}  // namespace rex::codegen
