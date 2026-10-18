/**
 * @file        tests/unit/codegen/midasm_hook_emit_test.cpp
 * @brief       The mid function hook wrapper skips the red zone and restores r1 on every exit
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
constexpr u32 kLiR3 = 0x38600001;
constexpr u32 kLiR4 = 0x38800002;
constexpr u32 kLiR5 = 0x38A00003;
constexpr u32 kHookIndex = 1;
constexpr u32 kBranchIndex = 2;
constexpr u32 kTargetIndex = 4;

constexpr u32 Address(u32 index) {
  return rex::test::kCodeImageBase + rex::test::kCodeImageCodeOffset + index * 4;
}

constexpr u32 Branch(u32 from_index, u32 to_index) {
  return 0x48000000 |
         ((static_cast<i32>(to_index) - static_cast<i32>(from_index)) * 4 & 0x03FFFFFC);
}

std::string GenerateWithHook(std::string_view hook_fields) {
  const std::vector<u32> words = {kLiR3, kLiR4, Branch(kBranchIndex, kTargetIndex), kLiR5, kBlr};
  const rex::test::PDataEntry pdata[] = {{Address(0), 5}};
  rex::test::TempFolder folder;
  const std::string manifest = std::format("[[entrypoint.midasm_hook]]\naddress = 0x{:X}\n{}\n",
                                           Address(kHookIndex), hook_fields);
  const auto result =
      rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(words, pdata), manifest);
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  const auto tree = rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
  const auto begin = tree.find("DEFINE_REX_FUNC(xstart)");
  REQUIRE(begin != std::string::npos);
  return tree.substr(begin, tree.find("\n}\n", begin) - begin);
}

size_t Count(const std::string& text, std::string_view needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
    ++count;
  }
  return count;
}

constexpr std::string_view kSkip = "ctx.r1.u64 -= rex::ppc::kRedZone;";
constexpr std::string_view kRestore = "ctx.r1.u64 += rex::ppc::kRedZone;";

}  // namespace

REX_TEST_CASE("A mid function hook skips the red zone before the call and restores r1 after it",
              "[codegen][midasm][redzone]") {
  const auto body = GenerateWithHook("name = \"PlainHook\"");
  const auto skip = body.find(kSkip);
  const auto call = body.find("PlainHook(");
  const auto restore = body.find(kRestore);
  REQUIRE(skip != std::string::npos);
  REQUIRE(call != std::string::npos);
  REQUIRE(restore != std::string::npos);
  CHECK(skip < call);
  CHECK(call < restore);
  CHECK(Count(body, kSkip) == 1);
  CHECK(Count(body, kRestore) == 1);
}

REX_TEST_CASE("A mid function hook that returns restores r1 before the return",
              "[codegen][midasm][redzone]") {
  const auto body = GenerateWithHook("name = \"ReturnHook\"\nreturn = true");
  const auto restore = body.find(kRestore);
  const auto early_return = body.find("return;", restore);
  REQUIRE(restore != std::string::npos);
  REQUIRE(early_return != std::string::npos);
  CHECK(Count(body, kSkip) == 1);
  CHECK(Count(body, kRestore) == 1);
}

REX_TEST_CASE("A mid function hook that jumps restores r1 before the jump",
              "[codegen][midasm][redzone]") {
  const auto body = GenerateWithHook(
      std::format("name = \"JumpHook\"\njump_address = 0x{:X}", Address(kTargetIndex)));
  const auto restore = body.find(kRestore);
  const auto jump = body.find(std::format("goto loc_{:X};", Address(kTargetIndex)), restore);
  REQUIRE(restore != std::string::npos);
  REQUIRE(jump != std::string::npos);
  CHECK(Count(body, kSkip) == 1);
  CHECK(Count(body, kRestore) == 1);
}

REX_TEST_CASE("A mid function hook with a condition restores r1 on both exits",
              "[codegen][midasm][redzone]") {
  const auto body = GenerateWithHook(
      std::format("name = \"BranchHook\"\nreturn_on_true = true\njump_address_on_false = 0x{:X}",
                  Address(kTargetIndex)));
  REQUIRE(body.find("if (BranchHook(") != std::string::npos);
  CHECK(Count(body, kSkip) == 1);
  CHECK(Count(body, kRestore) == 2);
  const auto taken = body.find(kRestore);
  const auto not_taken = body.find(kRestore, taken + 1);
  REQUIRE(not_taken != std::string::npos);
  CHECK(body.find("return;", taken) < not_taken);
  CHECK(body.find(std::format("goto loc_{:X};", Address(kTargetIndex)), not_taken) !=
        std::string::npos);
}
