/**
 * @file        tests/unit/codegen/skip_lr_test.cpp
 * @brief       skip_lr drops the LR traffic between guest functions and keeps it at import calls
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
constexpr u32 kMflrR0 = 0x7C0802A6;
constexpr u32 kLiR3One = 0x38600001;
constexpr u32 kLiR3Zero = 0x38600000;
constexpr u32 kMtctrR11 = 0x7D6903A6;
constexpr u32 kBctr = 0x4E800420;
constexpr u32 kImportFunctionRecord = 1u << 24;
constexpr u32 kKeDelayExecutionThread = 0x5A;
constexpr u32 kOwnerWords = 4;
constexpr u32 kImportCallIndex = 1;
constexpr u32 kGuestCallIndex = 2;
constexpr u32 kCalleeIndex = 4;
constexpr u32 kThunkIndex = 6;

constexpr u32 Address(u32 index) {
  return rex::test::kCodeImageBase + rex::test::kCodeImageCodeOffset + index * 4;
}

constexpr u32 BranchLink(u32 from_index, u32 to_index) {
  return 0x48000001 |
         ((static_cast<i32>(to_index) - static_cast<i32>(from_index)) * 4 & 0x03FFFFFC);
}

std::string GenerateOwner(std::string_view flags) {
  const std::vector<u32> words = {kMflrR0,
                                  BranchLink(kImportCallIndex, kThunkIndex),
                                  BranchLink(kGuestCallIndex, kCalleeIndex),
                                  kBlr,
                                  kLiR3One,
                                  kBlr,
                                  kImportFunctionRecord | kKeDelayExecutionThread,
                                  kLiR3Zero,
                                  kMtctrR11,
                                  kBctr};
  const rex::test::PDataEntry pdata[] = {{Address(0), kOwnerWords}, {Address(kCalleeIndex), 2}};
  rex::test::TempFolder folder;
  const auto result = rex::test::GenerateFromImage(
      folder, rex::test::BuildCodeImage(words, pdata), flags, true,
      {rex::test::XEXImportSpec{"xboxkrnl.exe", {Address(kThunkIndex)}, 0}});
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  const auto tree = rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
  const auto begin = tree.find("DEFINE_REX_FUNC(xstart)");
  REQUIRE(begin != std::string::npos);
  return tree.substr(begin, tree.find("\n}\n", begin) - begin);
}

bool StoresLR(const std::string& body, u32 return_index) {
  return body.find(std::format("ctx.lr = 0x{:X};", Address(return_index))) != std::string::npos;
}

}  // namespace

REX_TEST_CASE("An import call stores ctx.lr with skip_lr on", "[codegen][skip_lr]") {
  CHECK(StoresLR(GenerateOwner("skip_lr = true\n"), kImportCallIndex + 1));
}

REX_TEST_CASE("A call between guest functions has no ctx.lr store with skip_lr on",
              "[codegen][skip_lr]") {
  CHECK_FALSE(StoresLR(GenerateOwner("skip_lr = true\n"), kGuestCallIndex + 1));
}

REX_TEST_CASE("The mflr read is dropped with skip_lr on", "[codegen][skip_lr]") {
  CHECK(GenerateOwner("skip_lr = true\n").find("= ctx.lr;") == std::string::npos);
}

REX_TEST_CASE("Every call stores ctx.lr and mflr reads it with skip_lr off", "[codegen][skip_lr]") {
  const auto body = GenerateOwner("");
  CHECK(StoresLR(body, kImportCallIndex + 1));
  CHECK(StoresLR(body, kGuestCallIndex + 1));
  CHECK(body.find("= ctx.lr;") != std::string::npos);
}
