/**
 * @file        tests/unit/codegen/seh_scope_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <rex/codegen/codegen_context.h>
#include <rex/codegen/module_stub.h>
#include <rex/memory/byte_swap.h>

using namespace rex::codegen;
using rex::u32;
using rex::u8;

namespace {

constexpr u32 kBase = 0x82000000;
constexpr u32 kCompare = 0x2F030000;
constexpr u32 kBranchNotEqualMinus4 = 0x409AFFFC;
constexpr u32 kBlr = 0x4E800020;
constexpr u32 kMoveR1ToR31 = 0x7C3F0B78;

struct LoopFunction {
  std::vector<u8> data;
  StubModule module;
  std::unique_ptr<CodegenContext> ctx;
  FunctionNode* node = nullptr;

  LoopFunction(bool exception_handlers, bool seh_scope, bool non_volatile_as_local = false) {
    for (u32 word : {kMoveR1ToR31, kCompare, kBranchNotEqualMinus4, kBlr}) {
      data.resize(data.size() + 4);
      rex::memory::store<be_u32>(data.data() + data.size() - 4, word);
    }
    module.Load(kBase, data.data(), data.size());
    RecompilerConfig config;
    config.project_name = "testproj";
    config.generate_exception_handlers = exception_handlers;
    config.non_volatile_registers_as_local_variables = non_volatile_as_local;
    ctx = std::make_unique<CodegenContext>(
        CodegenContext::Create(BinaryView::FromModule(module), std::move(config)));
    node = ctx->graph.AddFunction(kBase, static_cast<u32>(data.size()),
                                  FunctionAuthority::kDiscovered, true);
    REQUIRE(node != nullptr);
    node->Discover({{kBase, static_cast<u32>(data.size())}}, {}, {});
    if (seh_scope) {
      SEHExceptionInfo seh;
      seh.scopes.push_back({kBase, kBase + 12, 0, 0});
      seh.frame_size = 0x60;
      ctx->graph.SetFunctionExceptionInfo(kBase, ExceptionInfo{seh});
    }
    node->Seal();
  }

  std::string Emit() const {
    EmitContext emit{ctx->GetBinary(), ctx->GetConfig(), ctx->graph, ctx->GetLimits(), 0};
    return node->EmitCpp(emit);
  }
};

}  // namespace

REX_TEST_CASE("An SEH function leaves its scope at every return", "[codegen][seh]") {
  const std::string code = LoopFunction(true, true).Emit();
  CHECK(code.find("REX_SEH_TRY") != std::string::npos);
  CHECK(code.find("REX_SEH_RETURN;") != std::string::npos);
  CHECK(code.find(" return;") == std::string::npos);
  CHECK(code.find("\treturn;") == std::string::npos);
}

REX_TEST_CASE("A function with SEH generation off returns plainly", "[codegen][seh]") {
  const std::string code = LoopFunction(false, true).Emit();
  CHECK(code.find("REX_SEH_RETURN") == std::string::npos);
  CHECK(code.find("return;") != std::string::npos);
}

REX_TEST_CASE("An SEH handler reads the frame register the body wrote", "[codegen][seh]") {
  const std::string code = LoopFunction(true, true, true).Emit();
  CHECK(code.find("PPCRegister r31{}") == std::string::npos);
  CHECK(code.find("ctx.r31.u64 = ctx.r1.u64;") != std::string::npos);
  CHECK(code.find("ctx.r12.s64 = ctx.r31.s64 + 96;") != std::string::npos);
}

REX_TEST_CASE("A function without SEH keeps its nonvolatile registers local", "[codegen][seh]") {
  const std::string code = LoopFunction(false, true, true).Emit();
  CHECK(code.find("PPCRegister r31{}") != std::string::npos);
}
