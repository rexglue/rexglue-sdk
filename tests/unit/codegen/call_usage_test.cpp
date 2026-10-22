/**
 * @file        tests/unit/codegen/call_usage_test.cpp
 * @brief       Codegen lists the argument registers each function reads at its entry
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "code_image.h"

#include <regex>
#include <string>
#include <vector>

namespace {

using rex::i32;
using rex::u32;

constexpr u32 kBlr = 0x4E800020;
constexpr u32 kBctrl = 0x4E800421;
constexpr u32 kBclNext = 0x429F0005;
constexpr u32 kLiR3Zero = 0x38600000;

constexpr u32 Address(u32 index) {
  return rex::test::kCodeImageBase + rex::test::kCodeImageCodeOffset + index * 4;
}

constexpr u32 XForm(u32 primary, u32 first, u32 second, u32 third, u32 extended) {
  return (primary << 26) | (first << 21) | (second << 16) | (third << 11) | (extended << 1);
}

constexpr u32 DForm(u32 primary, u32 first, u32 second, i32 immediate) {
  return (primary << 26) | (first << 21) | (second << 16) | (static_cast<u32>(immediate) & 0xFFFF);
}

constexpr u32 kAddR3R3R4 = XForm(31, 3, 3, 4, 266);
constexpr u32 kStwR5R3 = DForm(36, 5, 3, 0);
constexpr u32 kFaddF1F1F2 = XForm(63, 1, 1, 2, 21);
constexpr u32 kLwzuR5R3 = DForm(33, 5, 3, 4);
constexpr u32 kStmwR29R1 = DForm(47, 29, 1, -12);
constexpr u32 kLmwR3R1 = DForm(46, 3, 1, 0);
constexpr u32 kAddiR3R1 = DForm(14, 3, 1, 16);
constexpr u32 kLwzR4R13 = DForm(32, 4, 13, 0);
constexpr u32 kLwzR5R2 = DForm(32, 5, 2, 0);
constexpr u32 kRlwimiR4R3 = (20u << 26) | (3u << 21) | (4u << 16) | (15u << 1);

constexpr u32 OrR4(u32 source) {
  return XForm(31, source, 4, source, 444);
}

constexpr u32 Branch(u32 from_index, u32 to_index) {
  return 0x48000000 |
         ((static_cast<i32>(to_index) - static_cast<i32>(from_index)) * 4 & 0x03FFFFFC);
}

constexpr u32 BranchLink(u32 from_index, u32 to_index) {
  return Branch(from_index, to_index) | 1;
}

constexpr u32 Conditional(u32 options, u32 bit, u32 from_index, u32 to_index) {
  return (16u << 26) | (options << 21) | (bit << 16) |
         ((static_cast<i32>(to_index) - static_cast<i32>(from_index)) * 4 & 0xFFFC);
}

constexpr u32 kHelperIndex = 16;
constexpr u32 kSavedRegisterCount = 18;

std::vector<u32> WithSaveRun(std::vector<u32> leading) {
  leading.resize(kHelperIndex, 0);
  for (u32 reg = 14; reg < 14 + kSavedRegisterCount; ++reg) {
    leading.push_back(DForm(62, reg, 1, -8 * static_cast<i32>(33 - reg)));
  }
  leading.push_back(DForm(36, 12, 1, -8));
  leading.push_back(kBlr);
  return leading;
}

struct Usage {
  u32 gpr = 0;
  u32 fpr = 0;

  bool operator==(const Usage&) const = default;
};

constexpr u32 OrR3(u32 source) {
  return XForm(31, source, 3, source, 444);
}

Usage Analyze(const std::vector<u32>& words, const std::vector<rex::test::PDataEntry>& pdata,
              u32 index = 0) {
  rex::test::TempFolder folder;
  const auto result =
      rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(words, pdata), {}, true);
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  const auto tree = rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
  const auto begin = tree.find("kPPCCallUsage[] = {");
  REQUIRE(begin != std::string::npos);
  const std::string table = tree.substr(begin, tree.find("};", begin) - begin);
  const std::regex entry(R"(\{ 0x([0-9A-F]+), 0x([0-9A-F]+), 0x([0-9A-F]+) \})");
  for (auto it = std::sregex_iterator(table.begin(), table.end(), entry);
       it != std::sregex_iterator(); ++it) {
    if (std::stoul((*it)[1], nullptr, 16) == Address(index)) {
      return {static_cast<u32>(std::stoul((*it)[2], nullptr, 16)),
              static_cast<u32>(std::stoul((*it)[3], nullptr, 16))};
    }
  }
  return {};
}

Usage Single(const std::vector<u32>& words) {
  return Analyze(words, {{Address(0), static_cast<u32>(words.size())}});
}

}  // namespace

REX_TEST_CASE("call usage: an add reads both source registers", "[codegen][call_usage]") {
  CHECK(Single({kAddR3R3R4, kBlr}) == Usage{0x3, 0});
}

REX_TEST_CASE("call usage: reads of r1, r2 and r13 are not argument reads",
              "[codegen][call_usage]") {
  CHECK(Single({kAddiR3R1, kLwzR4R13, kLwzR5R2, kBlr}) == Usage{0, 0});
}

REX_TEST_CASE("call usage: a function that only writes r3 reads nothing", "[codegen][call_usage]") {
  CHECK(Single({kLiR3Zero, kBlr}) == Usage{0, 0});
}

REX_TEST_CASE("call usage: a store reads its source and its base", "[codegen][call_usage]") {
  CHECK(Single({kStwR5R3, kBlr}) == Usage{0x5, 0});
}

REX_TEST_CASE("call usage: a float add reads both float sources", "[codegen][call_usage]") {
  CHECK(Single({kFaddF1F1F2, kBlr}) == Usage{0, 0x3});
}

REX_TEST_CASE("call usage: a call defines the argument registers it reads nothing for",
              "[codegen][call_usage]") {
  const std::vector<u32> words = {BranchLink(0, 3), OrR4(3), kBlr, kBlr};
  const std::vector<rex::test::PDataEntry> pdata = {{Address(0), 3}, {Address(3), 1}};
  CHECK(Analyze(words, pdata) == Usage{0, 0});
}

REX_TEST_CASE("call usage: an indirect call defines the argument registers",
              "[codegen][call_usage]") {
  CHECK(Single({kBctrl, OrR4(3), kBlr}) == Usage{0, 0});
}

REX_TEST_CASE("call usage: a bcl to the next instruction is not a call", "[codegen][call_usage]") {
  CHECK(Single({kBclNext, OrR4(3), kBlr}) == Usage{0x1, 0});
}

REX_TEST_CASE("call usage: a call to a register save entry leaves the argument registers alone",
              "[codegen][call_usage]") {
  const auto words = WithSaveRun({BranchLink(0, kHelperIndex), OrR4(3), kBlr});
  CHECK(Analyze(words, {{Address(0), 3}}) == Usage{0x1, 0});
}

REX_TEST_CASE("call usage: a call to an ordinary function still defines the argument registers",
              "[codegen][call_usage]") {
  const auto words = WithSaveRun({BranchLink(0, 3), OrR4(3), kBlr, kBlr});
  CHECK(Analyze(words, {{Address(0), 3}, {Address(3), 1}}) == Usage{0, 0});
}

REX_TEST_CASE("call usage: a tail branch into a save entry is not a call",
              "[codegen][call_usage]") {
  const auto words = WithSaveRun({OrR4(3), Branch(1, kHelperIndex)});
  CHECK(Analyze(words, {{Address(0), 2}}) == Usage{0x1, 0});
}

REX_TEST_CASE("call usage: a bcl to the next instruction beside a save entry is not a call",
              "[codegen][call_usage]") {
  const auto words = WithSaveRun({kBclNext, OrR4(3), kBlr});
  CHECK(Analyze(words, {{Address(0), 3}}) == Usage{0x1, 0});
}

REX_TEST_CASE("call usage: a wrapper counts the argument registers its callee reads",
              "[codegen][call_usage]") {
  const std::vector<u32> words = {BranchLink(0, 3), kBlr, kBlr, OrR3(4), kBlr};
  CHECK(Analyze(words, {{Address(0), 3}, {Address(3), 2}}) == Usage{0x2, 0});
}

REX_TEST_CASE("call usage: a tail branch counts the argument registers its target reads",
              "[codegen][call_usage]") {
  const std::vector<u32> words = {Branch(0, 3), kBlr, kBlr, OrR3(4), kBlr};
  CHECK(Analyze(words, {{Address(0), 1}, {Address(3), 2}}) == Usage{0x2, 0});
}

REX_TEST_CASE("call usage: a wrapper of a wrapper counts the registers the last callee reads",
              "[codegen][call_usage]") {
  const std::vector<u32> words = {BranchLink(0, 3), kBlr, kBlr, BranchLink(3, 6), kBlr, kBlr,
                                  OrR3(5),          kBlr};
  CHECK(Analyze(words, {{Address(0), 3}, {Address(3), 3}, {Address(6), 2}}) == Usage{0x4, 0});
}

REX_TEST_CASE("call usage: a register the call defines before the read is not counted",
              "[codegen][call_usage]") {
  const std::vector<u32> words = {BranchLink(0, 3), OrR3(4), kBlr, kBlr};
  CHECK(Analyze(words, {{Address(0), 3}, {Address(3), 1}}) == Usage{0, 0});
}

REX_TEST_CASE("call usage: mutually recursive functions settle at the registers either reads",
              "[codegen][call_usage]") {
  const std::vector<u32> words = {OrR3(5), BranchLink(1, 3), kBlr, OrR3(6), BranchLink(4, 0), kBlr};
  const std::vector<rex::test::PDataEntry> pdata = {{Address(0), 3}, {Address(3), 3}};
  CHECK(Analyze(words, pdata, 0) == Usage{0xC, 0});
  CHECK(Analyze(words, pdata, 3) == Usage{0xC, 0});
}

REX_TEST_CASE("call usage: a function that calls itself terminates", "[codegen][call_usage]") {
  const std::vector<u32> words = {OrR3(4), BranchLink(1, 0), kBlr};
  CHECK(Single(words) == Usage{0x2, 0});
}

REX_TEST_CASE("call usage: an indirect call after a read keeps the read and defines the rest",
              "[codegen][call_usage]") {
  CHECK(Single({OrR3(4), kBctrl, OrR3(5), kBlr}) == Usage{0x2, 0});
}

REX_TEST_CASE("call usage: a call to a register save entry reads nothing from it",
              "[codegen][call_usage]") {
  const auto words = WithSaveRun({BranchLink(0, kHelperIndex), kBlr});
  CHECK(Analyze(words, {{Address(0), 2}}) == Usage{0, 0});
}

constexpr u32 kStwuR1 = DForm(37, 1, 1, -96);

std::vector<u32> VarargsPrologue(std::vector<u32> body) {
  std::vector<u32> words;
  for (u32 reg = 4; reg <= 10; ++reg) {
    words.push_back(DForm(62, reg, 1, 16 + 8 * static_cast<i32>(reg - 3)));
  }
  words.push_back(kStwuR1);
  words.insert(words.end(), body.begin(), body.end());
  return words;
}

REX_TEST_CASE("call usage: a varargs prologue that spills r4 to r10 reads only what the body reads",
              "[codegen][call_usage]") {
  CHECK(Single(VarargsPrologue({OrR4(3), kBlr})) == Usage{0x1, 0});
}

REX_TEST_CASE("call usage: a float spilled to the caller frame is not a read",
              "[codegen][call_usage]") {
  const u32 fadd_f1 = XForm(63, 1, 1, 1, 21);
  CHECK(Single({DForm(54, 2, 1, 24), kStwuR1, fadd_f1, kBlr}) == Usage{0, 0x1});
}

REX_TEST_CASE("call usage: a store through r1 after the frame is allocated is a read",
              "[codegen][call_usage]") {
  CHECK(Single({kStwuR1, DForm(36, 5, 1, 32), kBlr}) == Usage{0x4, 0});
}

REX_TEST_CASE("call usage: a function without a frame that spills r5 reads nothing",
              "[codegen][call_usage]") {
  CHECK(Single({DForm(36, 5, 1, 32), kLiR3Zero, kBlr}) == Usage{0, 0});
}

REX_TEST_CASE("call usage: a wrapper of a varargs function reads what the varargs body reads",
              "[codegen][call_usage]") {
  auto words = std::vector<u32>{BranchLink(0, 3), kBlr, kBlr};
  const auto callee = VarargsPrologue({OrR4(3), kBlr});
  words.insert(words.end(), callee.begin(), callee.end());
  CHECK(Analyze(words, {{Address(0), 3}, {Address(3), static_cast<u32>(callee.size())}}) ==
        Usage{0x1, 0});
}

REX_TEST_CASE("call usage: a branch where one path reads r6 reads r6", "[codegen][call_usage]") {
  const std::vector<u32> words = {Conditional(12, 2, 0, 3), OrR4(6), kBlr, kLiR3Zero, kBlr};
  CHECK(Single(words) == Usage{0x8, 0});
}

REX_TEST_CASE("call usage: a loop whose back edge leads to a read of r7 reads r7",
              "[codegen][call_usage]") {
  const std::vector<u32> words = {Branch(0, 3), OrR4(7), kBlr, Conditional(4, 2, 3, 1), kBlr};
  CHECK(Single(words) == Usage{0x10, 0});
}

REX_TEST_CASE("call usage: an insert rotate reads its destination", "[codegen][call_usage]") {
  CHECK(Single({kRlwimiR4R3, kBlr}) == Usage{0x3, 0});
}

REX_TEST_CASE("call usage: an update load reads and writes its base", "[codegen][call_usage]") {
  CHECK(Single({kLwzuR5R3, OrR4(3), kBlr}) == Usage{0x1, 0});
}

REX_TEST_CASE("call usage: a store multiple past r10 reads no argument register",
              "[codegen][call_usage]") {
  CHECK(Single({kStmwR29R1, kBlr}) == Usage{0, 0});
}

REX_TEST_CASE("call usage: a load multiple writes the argument registers",
              "[codegen][call_usage]") {
  CHECK(Single({kLmwR3R1, OrR4(3), kBlr}) == Usage{0, 0});
}
