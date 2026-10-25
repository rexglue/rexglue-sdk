/**
 * @file        tests/unit/codegen/stack_access_test.cpp
 * @brief       Accesses through r1 use the direct stack macros
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "code_image.h"

#include <string>
#include <string_view>
#include <vector>

namespace {

using rex::u32;

constexpr u32 kBlr = 0x4E800020;
constexpr u32 kLwzR3FromR1 = 0x80610008;
constexpr u32 kStwR3ToR1 = 0x90610008;
constexpr u32 kStwuR1 = 0x9421FFC0;
constexpr u32 kLfdF1FromR1 = 0xC8210010;
constexpr u32 kLwzxR3FromR1 = 0x7C61202E;
constexpr u32 kLwzR3FromR4 = 0x80640008;

std::string Generate(const std::vector<u32>& words) {
  const rex::test::PDataEntry pdata[] = {
      {rex::test::kCodeImageBase + rex::test::kCodeImageCodeOffset,
       static_cast<u32>(words.size())}};
  rex::test::TempFolder folder;
  const auto result =
      rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(words, pdata), "");
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  const auto tree = rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
  const auto begin = tree.find("DEFINE_REX_FUNC(xstart)");
  REQUIRE(begin != std::string::npos);
  return tree.substr(begin, tree.find("\n}\n", begin) - begin);
}

bool Has(const std::string& tree, std::string_view text) {
  return tree.find(text) != std::string::npos;
}

}  // namespace

REX_TEST_CASE("A load through r1 uses the stack macro", "[codegen][stack]") {
  const auto tree = Generate({kLwzR3FromR1, kBlr});
  CHECK(Has(tree, "REX_STACK_LOAD_U32"));
  CHECK_FALSE(Has(tree, "(REX_LOAD_U32"));
}

REX_TEST_CASE("A write through r1 uses the stack macro", "[codegen][stack]") {
  const auto tree = Generate({kStwR3ToR1, kBlr});
  CHECK(Has(tree, "REX_STACK_STORE_U32"));
}

REX_TEST_CASE("An update write through r1 uses the stack macro", "[codegen][stack]") {
  const auto tree = Generate({kStwuR1, kBlr});
  CHECK(Has(tree, "REX_STACK_STORE_U32"));
}

REX_TEST_CASE("A float load through r1 uses the stack macro", "[codegen][stack]") {
  const auto tree = Generate({kLfdF1FromR1, kBlr});
  CHECK(Has(tree, "REX_STACK_LOAD_U64"));
}

REX_TEST_CASE("An indexed load with r1 as the base uses the stack macro", "[codegen][stack]") {
  const auto tree = Generate({kLwzxR3FromR1, kBlr});
  CHECK(Has(tree, "REX_STACK_LOAD_U32"));
}

REX_TEST_CASE("A load through another base keeps the offset form", "[codegen][stack]") {
  const auto tree = Generate({kLwzR3FromR4, kBlr});
  CHECK(Has(tree, "REX_LOAD_U32"));
  CHECK_FALSE(Has(tree, "REX_STACK_LOAD"));
}
