/**
 * @file        tests/unit/codegen/setjmp_test.cpp
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
#include <vector>

namespace {

using rex::i32;
using rex::u32;

constexpr u32 kBlr = 0x4E800020;
constexpr u32 kLiR3Zero = 0x38600000;
constexpr u32 kMrR31R3 = 0x7C7F1B78;
constexpr u32 kMrR3R31 = 0x7FE3FB78;
constexpr u32 kFmrF31F1 = 0xFFE00890;
constexpr u32 kOwnerWords = 5;
constexpr u32 kSetjmpIndex = kOwnerWords;

constexpr u32 Address(u32 index) {
  return rex::test::kCodeImageBase + rex::test::kCodeImageCodeOffset + index * 4;
}

constexpr u32 BranchLink(u32 from_index, u32 to_index) {
  return 0x48000001 |
         ((static_cast<i32>(to_index) - static_cast<i32>(from_index)) * 4 & 0x03FFFFFC);
}

std::string GenerateOwner(std::string_view flags) {
  const std::vector<u32> words = {kMrR31R3,  kFmrF31F1, BranchLink(2, kSetjmpIndex), kMrR3R31, kBlr,
                                  kLiR3Zero, kBlr};
  const rex::test::PDataEntry pdata[] = {{Address(0), kOwnerWords}};
  rex::test::TempFolder folder;
  const auto result = rex::test::GenerateFromImage(
      folder, rex::test::BuildCodeImage(words, pdata),
      std::format("{}setjmp_address = 0x{:08X}\n", flags, Address(kSetjmpIndex)));
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  const auto tree = rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
  const auto begin = tree.find("DEFINE_REX_FUNC(xstart)");
  REQUIRE(begin != std::string::npos);
  return tree.substr(begin, tree.find("\n}\n", begin) - begin);
}

bool SnapshotsAndReloads(const std::string& body, std::string_view reg) {
  const auto snapshot = body.find("env = ctx;");
  const auto restore = body.find("ctx = env;");
  REQUIRE(snapshot != std::string::npos);
  REQUIRE(restore != std::string::npos);
  const auto stored = body.find(std::format("ctx.{0} = {0};", reg));
  const auto loaded = body.find(std::format("{0} = ctx.{0};", reg), restore);
  return stored != std::string::npos && stored < snapshot && loaded != std::string::npos;
}

}  // namespace

REX_TEST_CASE("A setjmp call writes r31 to ctx before the snapshot and reads it back",
              "[codegen][setjmp]") {
  CHECK(SnapshotsAndReloads(GenerateOwner("non_volatile_as_local = true\n"), "r31"));
}

REX_TEST_CASE("A setjmp call writes f31 to ctx before the snapshot and reads it back",
              "[codegen][setjmp]") {
  CHECK(SnapshotsAndReloads(GenerateOwner("non_volatile_as_local = true\n"), "f31"));
}

REX_TEST_CASE("A setjmp call in a function that keeps registers in ctx copies nothing",
              "[codegen][setjmp]") {
  const auto body = GenerateOwner("");
  CHECK(body.find("ctx.r31 = r31;") == std::string::npos);
  CHECK(body.find("r31 = ctx.r31;") == std::string::npos);
}
