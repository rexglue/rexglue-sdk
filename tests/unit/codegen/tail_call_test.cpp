/**
 * @file        tests/unit/codegen/tail_call_test.cpp
 * @brief       Branches out of a function are tail calls unless state must outlive the jump
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "code_image.h"

#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace {

using rex::i32;
using rex::u32;

constexpr u32 kBlr = 0x4E800020;
constexpr u32 kNop = 0x60000000;
constexpr u32 kMtctrR3 = 0x7C6903A6;
constexpr u32 kBctr = 0x4E800420;
constexpr u32 kCalleeIndex = 2;

constexpr u32 Address(u32 index) {
  return rex::test::kCodeImageBase + rex::test::kCodeImageCodeOffset + index * 4;
}

constexpr u32 Branch(u32 from_index, u32 to_index) {
  return 0x48000000 |
         ((static_cast<i32>(to_index) - static_cast<i32>(from_index)) * 4 & 0x03FFFFFC);
}

std::string Generate(const std::vector<u32>& words, std::string_view manifest = {}) {
  const rex::test::PDataEntry pdata[] = {{Address(0), 2}, {Address(kCalleeIndex), 1}};
  rex::test::TempFolder folder;
  const auto result =
      rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(words, pdata), manifest);
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  const auto tree = rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
  const auto begin = tree.find("DEFINE_REX_FUNC(xstart)");
  REQUIRE(begin != std::string::npos);
  return tree.substr(begin, tree.find("\n}\n", begin) - begin);
}

std::string HookAt(u32 index) {
  return std::format(
      "[[entrypoint.midasm_hook]]\naddress = 0x{:X}\nname = \"AfterHook\"\n"
      "after_instruction = true\n",
      Address(index));
}

bool Has(const std::string& tree, std::string_view text) {
  return tree.find(text) != std::string::npos;
}

std::string CalleeName() {
  return std::format("sub_{:08X}", Address(kCalleeIndex));
}

}  // namespace

REX_TEST_CASE("A branch to a function is a tail call", "[codegen][tailcall]") {
  const auto tree = Generate({Branch(0, kCalleeIndex), kNop, kBlr});
  CHECK(Has(tree, std::format("[[clang::musttail]] return {}(ctx, base);", CalleeName())));
}

REX_TEST_CASE("An indirect jump through the counter is a tail call", "[codegen][tailcall]") {
  const auto tree = Generate({kMtctrR3, kBctr, kBlr});
  CHECK(Has(tree, "REX_TAIL_INDIRECT_FUNC("));
  CHECK_FALSE(Has(tree, "REX_CALL_INDIRECT_FUNC(ctr"));
}

REX_TEST_CASE("A hook after a branch keeps the call and return form", "[codegen][tailcall]") {
  const auto tree = Generate({Branch(0, kCalleeIndex), kNop, kBlr}, HookAt(0));
  CHECK_FALSE(Has(tree, "musttail"));
  CHECK(Has(tree, std::format("{}(ctx, base);", CalleeName())));
}

REX_TEST_CASE("A hook after an indirect jump keeps the call and return form",
              "[codegen][tailcall]") {
  const auto tree = Generate({kMtctrR3, kBctr, kBlr}, HookAt(1));
  CHECK_FALSE(Has(tree, "REX_TAIL_INDIRECT_FUNC("));
  CHECK(Has(tree, "REX_CALL_INDIRECT_FUNC("));
}
