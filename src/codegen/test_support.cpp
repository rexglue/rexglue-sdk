#include <rex/codegen/test_support.h>

#include <algorithm>
#include <vector>

#include <format>

#include "ppc/instruction.h"

#include <rex/codegen/codegen_context.h>
#include <rex/logging.h>
#include <rex/memory/byte_swap.h>
#include <rex/runtime/binary_types.h>
#include <rex/types.h>

using rex::codegen::detail::decode_instruction;

namespace rex::codegen {

TestModule::TestModule() : Module(nullptr) {}

void TestModule::Load(u32 base_address, const u8* data, size_t size) {
  base_address_ = base_address;
  size_ = static_cast<u32>(size);

  binary_sections_.clear();
  binary_sections_.push_back(
      runtime::BinarySection{".text", base_address, static_cast<u32>(size), data, true, false});
}

void AnalyzeTestBinary(CodegenContext& ctx, std::string_view testName,
                       const std::map<size_t, std::string>& symbols, u32 baseAddress,
                       const u8* data, size_t dataSize) {
  std::vector<std::pair<size_t, std::string>> testFunctions;
  for (const auto& [addr, name] : symbols) {
    if (name.starts_with("test_")) {
      testFunctions.push_back({addr, name});
    }
  }

  std::sort(testFunctions.begin(), testFunctions.end());

  for (size_t i = 0; i < testFunctions.size(); i++) {
    u32 fnAddr = static_cast<u32>(testFunctions[i].first);

    u32 nextAddr = static_cast<u32>(baseAddress + dataSize);
    if (i + 1 < testFunctions.size()) {
      nextAddr = static_cast<u32>(testFunctions[i + 1].first);
    }
    u32 fnSize = nextAddr - fnAddr;

    auto* node = ctx.graph.addFunction(fnAddr, fnSize, FunctionAuthority::DISCOVERED, true);

    if (node) {
      node->discover({{fnAddr, fnSize}}, {}, {});
      ctx.graph.setFunctionName(fnAddr, std::format("{}_{:X}", testName, fnAddr));
      node->seal();
    }
  }

  for (size_t i = 0; i < testFunctions.size(); i++) {
    u32 fnAddr = static_cast<u32>(testFunctions[i].first);
    u32 nextAddr = static_cast<u32>(baseAddress + dataSize);
    if (i + 1 < testFunctions.size()) {
      nextAddr = static_cast<u32>(testFunctions[i + 1].first);
    }
    u32 fnSize = nextAddr - fnAddr;

    for (u32 offset = 0; offset < fnSize; offset += 4) {
      u32 pc = fnAddr + offset;
      u32 raw = memory::load<be_u32>(data + (fnAddr - baseAddress) + offset);
      auto decoded = decode_instruction(pc, raw);

      if (decoded.is_call() && decoded.branch_target.has_value()) {
        u32 target = decoded.branch_target.value();
        auto* targetNode = ctx.graph.getFunction(target);
        if (targetNode) {
          ctx.graph.addCallToFunction(fnAddr, pc, CallTarget::function(targetNode));
        }
      }
    }
  }
}

}  // namespace rex::codegen
