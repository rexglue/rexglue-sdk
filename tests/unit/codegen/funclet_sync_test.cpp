/**
 * @file        tests/unit/codegen/funclet_sync_test.cpp
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
constexpr u32 kLiR3One = 0x38600001;
constexpr u32 kMrR31R3 = 0x7C7F1B78;
constexpr u32 kMrR3R31 = 0x7FE3FB78;
constexpr u32 kFmrF31F1 = 0xFFE00890;
constexpr u32 kCmpwiCR5R3 = 0x2E830000;
constexpr u32 kStvx128V64 = 0x100B61CB;
constexpr u32 kVorV14 = 0x11CE7484;
constexpr u32 kVorV31 = 0x13FFFC84;
constexpr u32 kOwnerWords = 9;
constexpr u32 kFuncletIndex = kOwnerWords;

constexpr u32 Address(u32 index) {
  return rex::test::kCodeImageBase + rex::test::kCodeImageCodeOffset + index * 4;
}

constexpr u32 BranchLink(u32 from_index, u32 to_index) {
  return 0x48000001 |
         ((static_cast<i32>(to_index) - static_cast<i32>(from_index)) * 4 & 0x03FFFFFC);
}

std::string Body(const std::string& tree, std::string_view name) {
  const auto begin = tree.find(std::format("DEFINE_REX_FUNC({})", name));
  REQUIRE(begin != std::string::npos);
  return tree.substr(begin, tree.find("\n}\n", begin) - begin);
}

std::string GenerateOwner(std::string_view flags) {
  const std::vector<u32> words = {
      kMrR31R3, kFmrF31F1, kCmpwiCR5R3, kStvx128V64, kVorV14, kVorV31, BranchLink(6, kFuncletIndex),
      kMrR3R31, kBlr,      kLiR3One,    kBlr};
  const rex::test::PDataEntry pdata[] = {{Address(0), kOwnerWords}};
  rex::test::TempFolder folder;
  const auto result = rex::test::GenerateFromImage(
      folder, rex::test::BuildCodeImage(words, pdata),
      std::format("{}[entrypoint.functions]\n0x{:08X} = {{ share_registers = true }}\n", flags,
                  Address(kFuncletIndex)));
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  return Body(rex::test::ReadGeneratedTree(folder.path / "generated" / "entry"), "xstart");
}

bool SyncedAround(const std::string& body, std::string_view reg) {
  const auto call = body.find(std::format("sub_{:08X}(ctx, base);", Address(kFuncletIndex)));
  REQUIRE(call != std::string::npos);
  const auto before = body.find(std::format("ctx.{0} = {0};", reg));
  const auto after = body.find(std::format("{0} = ctx.{0};", reg), call);
  return before != std::string::npos && before < call && after != std::string::npos;
}

}  // namespace

REX_TEST_CASE("An owner copies r31 through ctx around a funclet call", "[codegen][funclet]") {
  CHECK(SyncedAround(GenerateOwner("non_volatile_as_local = true\n"), "r31"));
}

REX_TEST_CASE("An owner copies f31 through ctx around a funclet call", "[codegen][funclet]") {
  CHECK(SyncedAround(GenerateOwner("non_volatile_as_local = true\n"), "f31"));
}

REX_TEST_CASE("An owner copies v64 through ctx around a funclet call", "[codegen][funclet]") {
  CHECK(SyncedAround(GenerateOwner("non_volatile_as_local = true\n"), "v64"));
}

REX_TEST_CASE("An owner copies v14 through ctx around a funclet call", "[codegen][funclet]") {
  CHECK(SyncedAround(GenerateOwner("non_volatile_as_local = true\n"), "v14"));
}

REX_TEST_CASE("An owner copies v31 through ctx around a funclet call", "[codegen][funclet]") {
  CHECK(SyncedAround(GenerateOwner("non_volatile_as_local = true\n"), "v31"));
}

REX_TEST_CASE("An owner copies cr5 through ctx around a funclet call", "[codegen][funclet]") {
  CHECK(SyncedAround(GenerateOwner("non_volatile_as_local = true\ncr_as_local = true\n"), "cr5"));
}

REX_TEST_CASE("An owner that keeps registers in ctx copies nothing around a funclet call",
              "[codegen][funclet]") {
  const auto body = GenerateOwner("");
  CHECK(body.find("ctx.r31 = r31;") == std::string::npos);
  CHECK(body.find("r31 = ctx.r31;") == std::string::npos);
}
