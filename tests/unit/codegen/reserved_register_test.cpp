/**
 * @file        tests/unit/codegen/reserved_register_test.cpp
 * @brief       r2 and r13 stay in ctx whatever the localization flags say
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
constexpr u32 kLwzR3FromR2 = 0x80620000;
constexpr u32 kLwzR4FromR13 = 0x808D0000;
constexpr u32 kLwzR5FromR12 = 0x80AC0000;
constexpr u32 kFunctionWords = 4;

std::string Generate(std::string_view flags) {
  const std::vector<u32> words = {kLwzR3FromR2, kLwzR4FromR13, kLwzR5FromR12, kBlr};
  const rex::test::PDataEntry pdata[] = {
      {rex::test::kCodeImageBase + rex::test::kCodeImageCodeOffset, kFunctionWords}};
  rex::test::TempFolder folder;
  const auto result =
      rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(words, pdata), flags);
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  return rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
}

bool Has(const std::string& tree, std::string_view text) {
  return tree.find(text) != std::string::npos;
}

}  // namespace

REX_TEST_CASE("r2 stays in ctx under non_argument_as_local", "[codegen][registers]") {
  const auto tree = Generate("non_argument_as_local = true\n");
  CHECK(Has(tree, "ctx.r2.u32"));
  CHECK_FALSE(Has(tree, "PPCRegister r2{}"));
}

REX_TEST_CASE("r13 stays in ctx under non_volatile_as_local", "[codegen][registers]") {
  const auto tree = Generate("non_volatile_as_local = true\n");
  CHECK(Has(tree, "ctx.r13.u32"));
  CHECK_FALSE(Has(tree, "PPCRegister r13{}"));
}

REX_TEST_CASE("r12 is a local under non_argument_as_local", "[codegen][registers]") {
  const auto tree = Generate("non_argument_as_local = true\n");
  CHECK(Has(tree, "PPCRegister r12{}"));
}
