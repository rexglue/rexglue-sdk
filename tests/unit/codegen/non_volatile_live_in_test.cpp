/**
 * @file        tests/unit/codegen/non_volatile_live_in_test.cpp
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
#include "base/xex_builder.h"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include <rex/logging/logger.h>

namespace {

using rex::i32;
using rex::u32;

constexpr u32 kBlr = 0x4E800020;
constexpr u32 kMrR3R31 = 0x7FE3FB78;
constexpr u32 kLiR31One = 0x3BE00001;
constexpr u32 kStdR31R1 = 0xFBE1FFF0;
constexpr u32 kLdR31R1 = 0xEBE1FFF0;
constexpr u32 kStvx128V64 = 0x100B61CB;
constexpr u32 kVorV3V31 = 0x107FFC84;
constexpr u32 kXorR31R31R31 = 0x7FFFFA78;
constexpr u32 kBeqPlus8 = 0x41820008;

constexpr u32 Address(u32 index) {
  return rex::test::kCodeImageBase + rex::test::kCodeImageCodeOffset + index * 4;
}

constexpr u32 BranchLink(u32 from_index, u32 to_index) {
  return 0x48000001 |
         ((static_cast<i32>(to_index) - static_cast<i32>(from_index)) * 4 & 0x03FFFFFC);
}

struct Run {
  std::vector<std::string> lines;

  /** True when a log line says the function at index reads the registers. */
  bool Reports(u32 index, std::string_view registers) const {
    const std::string text = std::format("0x{:08X} reads {} before", Address(index), registers);
    return std::ranges::any_of(
        lines, [&](const std::string& line) { return line.find(text) != std::string::npos; });
  }

  bool ReportsAny() const {
    return std::ranges::any_of(lines, [](const std::string& line) {
      return line.find("before it writes them") != std::string::npos;
    });
  }
};

Run Generate(const std::vector<u32>& words, const std::vector<rex::test::PDataEntry>& pdata,
             std::string_view flags) {
  rex::test::TempFolder folder;
  rex::log::LogCaptureSink sink;
  const auto result = rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(words, pdata),
                                                   flags, true, {}, &sink);
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  std::vector<rex::log::CapturedMessage> messages;
  sink.CopySince(0, messages);
  Run run;
  for (const auto& message : messages) {
    run.lines.push_back(message.text);
  }
  return run;
}

Run Single(const std::vector<u32>& words, std::string_view flags) {
  return Generate(words, {{Address(0), static_cast<u32>(words.size())}}, flags);
}

constexpr std::string_view kLocals = "non_volatile_as_local = true\n";

}  // namespace

REX_TEST_CASE("live ins: a read of r31 before any write is reported", "[codegen][live_ins]") {
  CHECK(Single({kMrR3R31, kBlr}, kLocals).Reports(0, "r31"));
}

REX_TEST_CASE("live ins: a register written before it is read is not reported",
              "[codegen][live_ins]") {
  CHECK_FALSE(Single({kLiR31One, kMrR3R31, kBlr}, kLocals).ReportsAny());
}

REX_TEST_CASE("live ins: a register the function sets on another path is not reported",
              "[codegen][live_ins]") {
  CHECK_FALSE(Single({kBeqPlus8, kLiR31One, kMrR3R31, kBlr}, kLocals).ReportsAny());
}

REX_TEST_CASE("live ins: clearing a register with an xor of itself is not a read",
              "[codegen][live_ins]") {
  CHECK_FALSE(Single({kXorR31R31R31, kMrR3R31, kBlr}, kLocals).ReportsAny());
}

REX_TEST_CASE("live ins: a prologue save through r1 is not a read", "[codegen][live_ins]") {
  CHECK_FALSE(Single({kStdR31R1, kLiR31One, kMrR3R31, kLdR31R1, kBlr}, kLocals).ReportsAny());
}

REX_TEST_CASE("live ins: vector registers are reported", "[codegen][live_ins]") {
  CHECK(Single({kVorV3V31, kBlr}, kLocals).Reports(0, "v31"));
  CHECK(Single({kStvx128V64, kBlr}, kLocals).Reports(0, "v64"));
}

REX_TEST_CASE("live ins: a caller that does not set what its callee reads is reported too",
              "[codegen][live_ins]") {
  const std::vector<u32> words = {BranchLink(0, 2), kBlr, kMrR3R31, kBlr};
  const auto run = Generate(words, {{Address(0), 2}, {Address(2), 2}}, kLocals);
  CHECK(run.Reports(2, "r31"));
  CHECK(run.Reports(0, "r31"));
}

REX_TEST_CASE("live ins: a caller that sets what its callee reads is not reported",
              "[codegen][live_ins]") {
  const std::vector<u32> words = {kLiR31One, BranchLink(1, 3), kBlr, kMrR3R31, kBlr};
  const auto run = Generate(words, {{Address(0), 3}, {Address(3), 2}}, kLocals);
  CHECK(run.Reports(3, "r31"));
  CHECK_FALSE(run.Reports(0, "r31"));
}

REX_TEST_CASE("live ins: nothing is reported without non_volatile_as_local",
              "[codegen][live_ins]") {
  CHECK_FALSE(Single({kMrR3R31, kBlr}, "").ReportsAny());
}

REX_TEST_CASE("live ins: the warning names the function and its registers", "[codegen][live_ins]") {
  const auto run = Single({kMrR3R31, kBlr}, kLocals);
  const std::string warning = std::format("0x{:08X} reads r31 before it writes them", Address(0));
  CHECK(std::ranges::contains(run.lines, warning));
  CHECK(std::ranges::any_of(run.lines, [](const std::string& line) {
    return line.starts_with("1 functions read non volatile registers");
  }));
}
