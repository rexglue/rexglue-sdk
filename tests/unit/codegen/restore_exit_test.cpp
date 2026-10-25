/**
 * @file        tests/unit/codegen/restore_exit_test.cpp
 * @brief       A function that ends in a branch to a register restore helper exits through it
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "code_image.h"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <rex/logging/sink.h>

namespace {

using rex::i32;
using rex::u32;

constexpr u32 kBlr = 0x4E800020;
constexpr u32 kMtlrR12 = 0x7D8803A6;
constexpr u32 kFadd = 0xFC22182A;
constexpr u32 kVaddfp = 0x1020180A;
constexpr u32 kBneBackTwo = 0x4082FFF8;
constexpr u32 kLoopLength = 5;
constexpr u32 kHelperIndex = 16;
constexpr u32 kFirstRegister = 14;
constexpr u32 kRegisterCount = 18;

constexpr u32 DForm(u32 opcode, u32 rt, u32 ra, i32 disp) {
  return opcode << 26 | rt << 21 | ra << 16 | (static_cast<u32>(disp) & 0xFFFF);
}

constexpr u32 Address(u32 index) {
  return rex::test::kCodeImageBase + rex::test::kCodeImageCodeOffset + index * 4;
}

constexpr u32 Branch(u32 from_index, u32 to_index) {
  return 0x48000000 |
         ((static_cast<i32>(to_index) - static_cast<i32>(from_index)) * 4 & 0x03FFFFFC);
}

struct Output {
  std::string body;
  size_t run_offs = 0;
};

Output Generate(std::string_view manifest) {
  std::vector<u32> code = {kFadd, kFadd, kVaddfp, kBneBackTwo, Branch(4, kHelperIndex)};
  code.resize(kHelperIndex, 0);
  for (u32 reg = kFirstRegister; reg < kFirstRegister + kRegisterCount; ++reg) {
    code.push_back(DForm(58, reg, 1, -8 * static_cast<i32>(33 - reg)));
  }
  code.insert(code.end(), {DForm(32, 12, 1, -8), kMtlrR12, kBlr});
  const rex::test::PDataEntry pdata[] = {{Address(0), kLoopLength}};

  rex::log::CaptureSink sink;
  rex::test::TempFolder folder;
  const auto result = rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(code, pdata),
                                                   manifest, true, {}, &sink);
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);

  std::vector<rex::log::CapturedMessage> messages;
  sink.CopySince(0, messages);
  Output output;
  output.run_offs = static_cast<size_t>(
      std::ranges::count_if(messages, [](const rex::log::CapturedMessage& message) {
        return message.text.contains("runs off its end");
      }));
  const auto tree = rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
  const auto begin = tree.find("DEFINE_REX_FUNC(xstart)");
  REQUIRE(begin != std::string::npos);
  output.body = tree.substr(begin, tree.find("\n}\n", begin) - begin);
  return output;
}

bool Has(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

REX_TEST_CASE("A branch to a restore helper ends the function without a warning",
              "[codegen][millicode]") {
  const auto output = Generate({});
  INFO(output.body);
  CHECK(output.run_offs == 0);
  const auto call = output.body.find("__restgprlr_14(ctx, base);");
  REQUIRE(call != std::string::npos);
  CHECK(output.body.find("return;", call) != std::string::npos);
  CHECK_FALSE(Has(output.body, "musttail"));
}

REX_TEST_CASE(
    "A branch to a restore helper under non_volatile_as_local ends the function without a warning",
    "[codegen][millicode]") {
  const auto output = Generate("non_volatile_as_local = true\n");
  INFO(output.body);
  CHECK(output.run_offs == 0);
  CHECK_FALSE(Has(output.body, "__restgprlr_14(ctx, base)"));
  CHECK(Has(output.body, "return;"));
  CHECK_FALSE(Has(output.body, "musttail"));
}
