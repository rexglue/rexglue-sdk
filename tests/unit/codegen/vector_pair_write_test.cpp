/**
 * @file        tests/unit/codegen/vector_pair_write_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "code_image.h"
#include "rex_test.h"
#include "temp_folder.h"

#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace {

using rex::u32;

constexpr u32 kBlr = 0x4E800020;
constexpr u32 kStvlxV0 = 0x7C035D0E;
constexpr u32 kStvrxV0 = 0x7C03654E;
constexpr u32 kStvrxV1 = 0x7C23654E;
constexpr u32 kBeqToThird = 0x41820008;

constexpr u32 Address(u32 index) {
  return rex::test::kCodeImageBase + rex::test::kCodeImageCodeOffset + index * 4;
}

std::string Generate(const std::vector<u32>& words, std::string_view manifest = {}) {
  const rex::test::PDataEntry pdata[] = {{Address(0), static_cast<u32>(words.size())}};
  rex::test::TempFolder folder;
  const auto result =
      rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(words, pdata), manifest);
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  return rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
}

bool Has(const std::string& tree, std::string_view text) {
  return tree.find(text) != std::string::npos;
}

size_t Count(const std::string& text, std::string_view needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
    ++count;
  }
  return count;
}

std::string HookAt(u32 index, bool after) {
  return std::format("[[entrypoint.midasm_hook]]\naddress = 0x{:X}\nname = \"PairHook\"\n{}",
                     Address(index), after ? "after_instruction = true\n" : "");
}

}  // namespace

REX_TEST_CASE("stvlx followed by stvrx of the same vector writes the pair at once",
              "[codegen][vector]") {
  const auto tree = Generate({kStvlxV0, kStvrxV0, kBlr});
  CHECK(Has(tree, "// stvlx v0,r3,r11"));
  CHECK(Has(tree, "// stvrx v0,r3,r12"));
  CHECK(Count(tree, "StoreVectorPair(") == 1);
  CHECK_FALSE(Has(tree, "StoreVectorLeft("));
  CHECK_FALSE(Has(tree, "StoreVectorRight("));
  CHECK(Has(tree, "REX_RAW_ADDR(temp.u32 & ~0xF), temp.u32"));
}

REX_TEST_CASE("stvlx followed by stvrx of another vector keeps two writes", "[codegen][vector]") {
  const auto tree = Generate({kStvlxV0, kStvrxV1, kBlr});
  CHECK(Has(tree, "// stvrx v1,r3,r12"));
  CHECK(Count(tree, "StoreVectorLeft(") == 1);
  CHECK(Count(tree, "StoreVectorRight(") == 1);
  CHECK_FALSE(Has(tree, "StoreVectorPair("));
}

REX_TEST_CASE("a label between stvlx and stvrx keeps two writes", "[codegen][vector]") {
  const auto tree = Generate({kBeqToThird, kStvlxV0, kStvrxV0, kBlr});
  CHECK(Has(tree, "loc_"));
  CHECK(Count(tree, "StoreVectorLeft(") == 1);
  CHECK(Count(tree, "StoreVectorRight(") == 1);
  CHECK_FALSE(Has(tree, "StoreVectorPair("));
}

REX_TEST_CASE("a mid asm hook on the stvrx keeps two writes", "[codegen][vector]") {
  const auto tree = Generate({kStvlxV0, kStvrxV0, kBlr}, HookAt(1, false));
  CHECK(Has(tree, "PairHook"));
  CHECK(Count(tree, "StoreVectorLeft(") == 1);
  CHECK(Count(tree, "StoreVectorRight(") == 1);
  CHECK_FALSE(Has(tree, "StoreVectorPair("));
}

REX_TEST_CASE("a mid asm hook after the stvlx keeps two writes", "[codegen][vector]") {
  const auto tree = Generate({kStvlxV0, kStvrxV0, kBlr}, HookAt(0, true));
  CHECK(Has(tree, "PairHook"));
  CHECK(Count(tree, "StoreVectorLeft(") == 1);
  CHECK(Count(tree, "StoreVectorRight(") == 1);
  CHECK_FALSE(Has(tree, "StoreVectorPair("));
}

REX_TEST_CASE("a lone stvlx uses the left write and a lone stvrx the right write",
              "[codegen][vector]") {
  const auto left = Generate({kStvlxV0, kBlr});
  CHECK(Count(left, "StoreVectorLeft(") == 1);
  CHECK_FALSE(Has(left, "StoreVectorPair("));
  const auto right = Generate({kStvrxV0, kBlr});
  CHECK(Count(right, "StoreVectorRight(") == 1);
  CHECK_FALSE(Has(right, "StoreVectorPair("));
}
