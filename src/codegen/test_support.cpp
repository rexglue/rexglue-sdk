#include <rex/codegen/test_support.h>

#include <algorithm>
#include <vector>

#include <format>

#include "ppc/instruction.h"

#include <rex/codegen/codegen_context.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime/binary_types.h>
#include <rex/types.h>

using rex::codegen::ppc::decode_instruction;

namespace rex::codegen {

TestModule::TestModule() : Module(nullptr) {}

void TestModule::Load(uint32_t base_address, const uint8_t* data, size_t size) {
  base_address_ = base_address;
  size_ = static_cast<uint32_t>(size);

  binary_sections_.clear();
  binary_sections_.push_back(runtime::BinarySection{
      ".text", base_address, static_cast<uint32_t>(size), data, true, false});
}

void AnalyzeTestBinary(CodegenContext& ctx, std::string_view testName,
                       const std::map<size_t, std::string>& symbols, uint32_t baseAddress,
                       const uint8_t* data, size_t dataSize) {
  std::vector<std::pair<size_t, std::string>> testFunctions;
  for (const auto& [addr, name] : symbols) {
    if (name.starts_with("test_")) {
      testFunctions.push_back({addr, name});
    }
  }

  std::sort(testFunctions.begin(), testFunctions.end());

  for (size_t i = 0; i < testFunctions.size(); i++) {
    uint32_t fnAddr = static_cast<uint32_t>(testFunctions[i].first);

    uint32_t nextAddr = static_cast<uint32_t>(baseAddress + dataSize);
    if (i + 1 < testFunctions.size()) {
      nextAddr = static_cast<uint32_t>(testFunctions[i + 1].first);
    }
    uint32_t fnSize = nextAddr - fnAddr;

    auto* node = ctx.graph.addFunction(fnAddr, fnSize, FunctionAuthority::DISCOVERED, true);

    if (node) {
      node->discover({{fnAddr, fnSize}}, {}, {});
      ctx.graph.setFunctionName(fnAddr, std::format("{}_{:X}", testName, fnAddr));
      node->seal();
    }
  }

  for (size_t i = 0; i < testFunctions.size(); i++) {
    uint32_t fnAddr = static_cast<uint32_t>(testFunctions[i].first);
    uint32_t nextAddr = static_cast<uint32_t>(baseAddress + dataSize);
    if (i + 1 < testFunctions.size()) {
      nextAddr = static_cast<uint32_t>(testFunctions[i + 1].first);
    }
    uint32_t fnSize = nextAddr - fnAddr;

    for (uint32_t offset = 0; offset < fnSize; offset += 4) {
      uint32_t pc = fnAddr + offset;
      uint32_t raw = memory::load<be_u32>(data + (fnAddr - baseAddress) + offset);
      auto decoded = decode_instruction(pc, raw);

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
