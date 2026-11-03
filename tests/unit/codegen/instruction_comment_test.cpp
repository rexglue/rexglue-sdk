/**
 * @file        tests/unit/codegen/instruction_comment_test.cpp
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

#include <algorithm>
#include <string>
#include <vector>

namespace {

using rex::u32;

constexpr u32 kBlr = 0x4E800020;
constexpr u32 kNop = 0x60000000;
constexpr u32 kAddR3R4R5 = 0x7C642A14;

std::string Generate(const std::vector<u32>& words) {
  const rex::test::PDataEntry pdata[] = {
      {rex::test::kCodeImageBase + rex::test::kCodeImageCodeOffset,
       static_cast<u32>(words.size())}};
  rex::test::TempFolder folder;
  const auto result =
      rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(words, pdata), "");
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  return rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
}

bool IsPlainText(const std::string& text) {
  return std::ranges::all_of(text, [](char c) {
    const auto byte = static_cast<unsigned char>(c);
    return byte == '\n' || byte == '\t' || byte == '\r' || (byte >= 0x20 && byte < 0x7F);
  });
}

}  // namespace

REX_TEST_CASE("An instruction without operands writes a comment with no stray bytes",
              "[codegen][comment]") {
  const auto tree = Generate({kBlr});
  CHECK(IsPlainText(tree));
  CHECK(tree.find("// blr") != std::string::npos);
}

REX_TEST_CASE("Instruction comments end at the disassembled text", "[codegen][comment]") {
  const auto tree = Generate({kAddR3R4R5, kNop, kBlr});
  CHECK(IsPlainText(tree));
  CHECK(tree.find("// add r3,r4,r5") != std::string::npos);
}
