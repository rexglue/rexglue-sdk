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

using rex::codegen::detail::DecodeInstruction;

namespace rex::codegen {

TestModule::TestModule() : Module(nullptr) {}

void TestModule::Load(u32 base_address, const u8* data, size_t size) {
  base_address_ = base_address;
  size_ = static_cast<u32>(size);

  binary_sections_.clear();
  binary_sections_.push_back(
      runtime::BinarySection{".text", base_address, static_cast<u32>(size), data, true, false});
}

void AnalyzeTestBinary(CodegenContext& ctx, std::string_view test_name,
                       const std::map<size_t, std::string>& symbols, u32 base_address,
                       const u8* data, size_t data_size) {
  std::vector<std::pair<size_t, std::string>> test_functions;
  for (const auto& [addr, name] : symbols) {
    if (name.starts_with("test_")) {
      test_functions.push_back({addr, name});
    }
  }

  std::sort(test_functions.begin(), test_functions.end());

  for (size_t i = 0; i < test_functions.size(); i++) {
    u32 fn_addr = static_cast<u32>(test_functions[i].first);

    u32 next_addr = static_cast<u32>(base_address + data_size);
    if (i + 1 < test_functions.size()) {
      next_addr = static_cast<u32>(test_functions[i + 1].first);
    }
    u32 fn_size = next_addr - fn_addr;

    auto* node = ctx.graph.AddFunction(fn_addr, fn_size, FunctionAuthority::kDiscovered, true);

    if (node) {
      node->Discover({{fn_addr, fn_size}}, {}, {});
      ctx.graph.SetFunctionName(fn_addr, std::format("{}_{:X}", test_name, fn_addr));
      node->Seal();
    }
  }

  for (size_t i = 0; i < test_functions.size(); i++) {
    u32 fn_addr = static_cast<u32>(test_functions[i].first);
    u32 next_addr = static_cast<u32>(base_address + data_size);
    if (i + 1 < test_functions.size()) {
      next_addr = static_cast<u32>(test_functions[i + 1].first);
    }
    u32 fn_size = next_addr - fn_addr;

    for (u32 offset = 0; offset < fn_size; offset += 4) {
      u32 pc = fn_addr + offset;
      u32 raw = memory::load<be_u32>(data + (fn_addr - base_address) + offset);
      auto decoded = DecodeInstruction(pc, raw);

      if (decoded.IsCall() && decoded.branch_target.has_value()) {
        u32 target = decoded.branch_target.value();
        auto* target_node = ctx.graph.GetFunction(target);
        if (target_node) {
          ctx.graph.AddCallToFunction(fn_addr, pc, CallTarget::MakeFunction(target_node));
        }
      }
    }
  }
}

}  // namespace rex::codegen
