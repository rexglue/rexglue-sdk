/**
 * @file        tests/unit/codegen/function_scanner_test.cpp
 * @brief       Block discovery stops where another function begins
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <rex/codegen/codegen_context.h>
#include <rex/codegen/config.h>
#include <rex/codegen/function_scanner.h>
#include <rex/codegen/test_support.h>
#include <rex/memory/byte_swap.h>

#include "codegen/decoded_binary.h"

using namespace rex::codegen;
using rex::u32;
using rex::u8;

namespace {

constexpr u32 kBase = 0x82000000;
constexpr u32 kNop = 0x60000000;
constexpr u32 kBlr = 0x4E800020;

struct ScannerFixture {
  std::vector<u8> data;
  TestModule module;
  std::unique_ptr<CodegenContext> ctx;

  explicit ScannerFixture(std::initializer_list<u32> words) {
    for (u32 word : words) {
      data.resize(data.size() + 4);
      rex::memory::store<be_u32>(data.data() + data.size() - 4, word);
    }
    module.Load(kBase, data.data(), data.size());
    RecompilerConfig config;
    config.project_name = "testproj";
    ctx = std::make_unique<CodegenContext>(
        CodegenContext::Create(BinaryView::FromModule(module), std::move(config)));
    ctx->InitDecoded();
  }

  BlockDiscoveryResult Discover(const std::unordered_set<u32>& known_functions) const {
    const CodeRegion* region = ctx->GetDecoded().FindRegionContaining(kBase);
    REQUIRE(region != nullptr);
    return DiscoverBlocks(ctx->GetDecoded(), ctx->GetLimits(), kBase, *region, known_functions);
  }
};

}  // namespace

REX_TEST_CASE("Block discovery does not run into the next function", "[codegen][scanner]") {
  ScannerFixture fixture({kNop, kNop, kNop, kNop, kBlr});
  const BlockDiscoveryResult result = fixture.Discover({kBase, kBase + 8});
  REQUIRE(result.blocks.size() == 1);
  CHECK(result.blocks[0].base == kBase);
  CHECK(result.blocks[0].size == 8);
  CHECK(result.instructions.size() == 2);
}

REX_TEST_CASE("Block discovery continues through addresses that are not function entries",
              "[codegen][scanner]") {
  ScannerFixture fixture({kNop, kNop, kNop, kNop, kBlr});
  const BlockDiscoveryResult result = fixture.Discover({kBase});
  REQUIRE(result.blocks.size() == 1);
  CHECK(result.blocks[0].size == 20);
  CHECK(result.instructions.size() == 5);
}

REX_TEST_CASE("A function that runs into the next function ends with a tail call to it",
              "[codegen][scanner]") {
  ScannerFixture fixture({kNop, kNop, kBlr});
  auto& graph = fixture.ctx->graph;
  FunctionNode* first = graph.AddFunction(kBase, 8, FunctionAuthority::kDiscovered, true);
  FunctionNode* second = graph.AddFunction(kBase + 8, 4, FunctionAuthority::kDiscovered, true);
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);

  BlockDiscoveryResult result = fixture.Discover({kBase, kBase + 8});
  for (const auto& branch : result.unresolved_branches) {
    graph.AddUnresolvedJumpToFunction(kBase, branch.site, branch.target, branch.is_call,
                                      branch.is_conditional);
  }
  first->Discover(std::move(result.blocks), std::move(result.instructions),
                  std::move(result.labels));
  first->SetFallThroughs(std::move(result.fall_throughs));
  first->Seal();

  const EmitContext emit{fixture.ctx->GetBinary(), fixture.ctx->GetConfig(), graph,
                         fixture.ctx->GetLimits(), 0};
  const std::string code = first->EmitCpp(emit);
  const size_t call = code.find("sub_82000008(ctx, base);");
  REQUIRE(call != std::string::npos);
  CHECK(code.find("return;", call) != std::string::npos);
}

REX_TEST_CASE("A conditional branch to another function keeps its condition",
              "[codegen][scanner]") {
  constexpr u32 kBranchEqualPlus12 = 0x419A000C;
  ScannerFixture fixture({kBranchEqualPlus12, kNop, kNop, kBranchEqualPlus12, kBlr, kNop, kBlr});
  auto& graph = fixture.ctx->graph;
  FunctionNode* first = graph.AddFunction(kBase, 20, FunctionAuthority::kDiscovered, true);
  FunctionNode* second = graph.AddFunction(kBase + 24, 4, FunctionAuthority::kDiscovered, true);
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);

  BlockDiscoveryResult result = fixture.Discover({kBase, kBase + 24});
  for (const auto& branch : result.unresolved_branches) {
    graph.AddUnresolvedJumpToFunction(kBase, branch.site, branch.target, branch.is_call,
                                      branch.is_conditional);
  }
  first->Discover(std::move(result.blocks), std::move(result.instructions),
                  std::move(result.labels));
  first->SetFallThroughs(std::move(result.fall_throughs));
  first->Seal();

  const EmitContext emit{fixture.ctx->GetBinary(), fixture.ctx->GetConfig(), graph,
                         fixture.ctx->GetLimits(), 0};
  const std::string code = first->EmitCpp(emit);
  size_t calls = 0;
  for (size_t at = code.find("sub_82000018(ctx, base);"); at != std::string::npos;
       at = code.find("sub_82000018(ctx, base);", at + 1)) {
    ++calls;
  }
  CHECK(calls == 1);
  CHECK(code.find("if (") != std::string::npos);
}

REX_TEST_CASE("A function with two fall throughs calls both next functions", "[codegen][scanner]") {
  constexpr u32 kBranchEqualPlus16 = 0x419A0010;
  ScannerFixture fixture({kBranchEqualPlus16, kNop, kNop, kBlr, kNop, kNop, kBlr});
  auto& graph = fixture.ctx->graph;
  FunctionNode* first = graph.AddFunction(kBase, 24, FunctionAuthority::kDiscovered, true);
  FunctionNode* inner = graph.AddFunction(kBase + 12, 4, FunctionAuthority::kDiscovered, true);
  FunctionNode* last = graph.AddFunction(kBase + 24, 4, FunctionAuthority::kDiscovered, true);
  REQUIRE(first != nullptr);
  REQUIRE(inner != nullptr);
  REQUIRE(last != nullptr);

  BlockDiscoveryResult result = fixture.Discover({kBase, kBase + 12, kBase + 24});
  CHECK(result.fall_throughs == std::set<u32>{kBase + 12, kBase + 24});
  for (const auto& branch : result.unresolved_branches) {
    graph.AddUnresolvedJumpToFunction(kBase, branch.site, branch.target, branch.is_call,
                                      branch.is_conditional);
  }
  first->Discover(std::move(result.blocks), std::move(result.instructions),
                  std::move(result.labels));
  first->SetFallThroughs(std::move(result.fall_throughs));
  first->Seal();

  const EmitContext emit{fixture.ctx->GetBinary(), fixture.ctx->GetConfig(), graph,
                         fixture.ctx->GetLimits(), 0};
  const std::string code = first->EmitCpp(emit);
  for (const char* call : {"sub_8200000C(ctx, base);", "sub_82000018(ctx, base);"}) {
    INFO(call);
    const size_t at = code.find(call);
    REQUIRE(at != std::string::npos);
    CHECK(code.find("return;", at) != std::string::npos);
  }
}

REX_TEST_CASE("Reading the decoded binary swaps big endian values of every width",
              "[codegen][scanner]") {
  ScannerFixture fixture({0x01020304, 0x05060708});
  const DecodedBinary& decoded = fixture.ctx->GetDecoded();
  CHECK(decoded.Read<u8>(kBase) == 0x01);
  CHECK(decoded.Read<u16>(kBase) == 0x0102);
  CHECK(decoded.Read<u32>(kBase) == 0x01020304);
  CHECK(decoded.Read<u64>(kBase) == 0x0102030405060708ULL);
  CHECK_FALSE(decoded.Read<u32>(kBase + 8).has_value());
}
