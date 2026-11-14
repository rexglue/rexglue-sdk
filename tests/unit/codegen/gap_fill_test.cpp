/**
 * @file        tests/unit/codegen/gap_fill_test.cpp
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

#include <format>
#include <string>
#include <vector>

namespace {

using rex::i32;
using rex::u32;

constexpr u32 kBlr = 0x4E800020;
constexpr u32 kBctr = 0x4E800420;
constexpr u32 kLiR3One = 0x38600001;
constexpr u32 kLiR3Two = 0x38600002;
constexpr u32 kLiR3Three = 0x38600003;
constexpr u32 kLiR3Zero = 0x38600000;
constexpr u32 kAddiR3Eight = 0x38630008;
constexpr u32 kLwzR3 = 0x80630000;
constexpr u32 kLwzR11FromR3 = 0x81630000;
constexpr u32 kLwzR10From12R11 = 0x814B000C;
constexpr u32 kMtctrR10 = 0x7D4903A6;
constexpr u32 kCmplwiCR6R3Zero = 0x2B030000;
constexpr u32 kBeqCR6 = 0x419A0000;
constexpr u32 kStdR31Red = 0xFBE1FFF8;
constexpr u32 kLdR31Red = 0xEBE1FFF8;
constexpr u32 kMrR31R3 = 0x7C7F1B78;
constexpr u32 kAddiR3FromR31 = 0x387F0001;
constexpr u32 kLwzR16FromR9 = 0x8209EEC8;
constexpr u32 kLwzR18FromR27 = 0x8259EF68;
constexpr u32 kTwiAlways = 0x0FE00016;
constexpr u32 kStwR31ToR3 = 0x93E30000;
constexpr u32 kStwR31ToR1 = 0x93E10010;
constexpr u32 kStwR18ToR27 = 0x9259EF6C;
constexpr u32 kSlwiR10R3Two = 0x5460103A;
constexpr u32 kLwzxR10R11R10 = 0x7D4B502E;

constexpr u32 Address(u32 index) {
  return rex::test::kCodeImageBase + rex::test::kCodeImageCodeOffset + index * 4;
}

constexpr u32 Branch(u32 from_index, u32 to_index) {
  return 0x48000000 |
         ((static_cast<i32>(to_index) - static_cast<i32>(from_index)) * 4 & 0x03FFFFFC);
}

std::string Generate(const std::vector<u32>& words, u32 pdata_length = 2) {
  rex::test::TempFolder folder;
  const rex::test::PDataEntry pdata[] = {{Address(0), pdata_length}};
  const auto result = rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(words, pdata));
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  return rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
}

bool HasFunction(const std::string& tree, u32 index) {
  return tree.find(std::format("DEFINE_REX_FUNC(sub_{:08X})", Address(index))) != std::string::npos;
}

}  // namespace

REX_TEST_CASE("A lone return after a return is its own function", "[codegen][gapfill]") {
  const auto tree = Generate({kLiR3One, kBlr, kBlr, kBlr});
  CHECK(HasFunction(tree, 2));
  CHECK(HasFunction(tree, 3));
}

REX_TEST_CASE("A run of adjustor thunks gives each thunk an entry", "[codegen][gapfill]") {
  const auto tree = Generate({kLiR3One, kBlr, kLwzR3, kBlr, kAddiR3Eight, Branch(5, 2),
                              kAddiR3Eight, Branch(7, 2), kAddiR3Eight, Branch(9, 2)});
  CHECK(HasFunction(tree, 2));
  CHECK(HasFunction(tree, 4));
  CHECK(HasFunction(tree, 6));
  CHECK(HasFunction(tree, 8));
}

REX_TEST_CASE("A thunk through the count register is split from the leaf after it",
              "[codegen][gapfill]") {
  const auto tree =
      Generate({kLiR3One, kBlr, kLwzR11FromR3, kLwzR10From12R11, kMtctrR10, kBctr, kLiR3One, kBlr});
  CHECK(HasFunction(tree, 2));
  CHECK(HasFunction(tree, 6));
}

REX_TEST_CASE("A leaf that starts at the end of a pdata extent is registered",
              "[codegen][gapfill]") {
  const auto tree = Generate({kLiR3One, Branch(1, 0), kLiR3Two, kBlr});
  CHECK(HasFunction(tree, 2));
}

REX_TEST_CASE("A frameless function with an inner branch stays 1 function", "[codegen][gapfill]") {
  const auto tree =
      Generate({kLiR3One, kBlr, kCmplwiCR6R3Zero, kBeqCR6 | 12, kLiR3One, kBlr, kLiR3Zero, kBlr});
  CHECK(HasFunction(tree, 2));
  CHECK_FALSE(HasFunction(tree, 6));
}

REX_TEST_CASE("A detected jump table keeps the case bodies after the table in the function",
              "[codegen][gapfill]") {
  constexpr u32 kTable = 8;
  constexpr u32 kFirstCase = 10;
  constexpr u32 kSecondCase = 12;
  const u32 table = Address(kTable);
  const u32 hi = (table + 0x8000) >> 16;
  const u32 lo = table & 0xFFFF;
  const auto tree = Generate({kLiR3One, kBlr, 0x3D600000 | hi, 0x396B0000 | lo, kSlwiR10R3Two,
                              kLwzxR10R11R10, kMtctrR10, kBctr, Address(kFirstCase),
                              Address(kSecondCase), kLiR3One, kBlr, kLiR3Two, kBlr});
  CHECK(HasFunction(tree, 2));
  CHECK_FALSE(HasFunction(tree, kFirstCase));
  CHECK_FALSE(HasFunction(tree, kSecondCase));
}

REX_TEST_CASE("Padding between functions is not a function", "[codegen][gapfill]") {
  const auto tree = Generate({kLiR3One, kBlr, 0, 0, kLiR3Two, kBlr});
  CHECK(HasFunction(tree, 4));
  CHECK_FALSE(HasFunction(tree, 2));
  CHECK_FALSE(HasFunction(tree, 3));
}

REX_TEST_CASE("A load through an unwritten nonvolatile base is data", "[codegen][gapfill]") {
  const auto tree = Generate({kLiR3One, kBlr, kLwzR18FromR27, kStwR18ToR27, kBlr});
  CHECK_FALSE(HasFunction(tree, 2));
  CHECK_FALSE(HasFunction(tree, 3));
}

REX_TEST_CASE("A red zone leaf that saves r31 first is a function", "[codegen][gapfill]") {
  const auto tree = Generate({kLiR3One, kBlr, kStdR31Red, kLiR3One, kLdR31Red, kBlr});
  CHECK(HasFunction(tree, 2));
}

REX_TEST_CASE("A pointer word that decodes as a load into a nonvolatile register is data",
              "[codegen][gapfill]") {
  const auto tree = Generate({kLiR3One, kBlr, kLwzR16FromR9, kBlr});
  CHECK_FALSE(HasFunction(tree, 2));
}

REX_TEST_CASE("A write to a nonvolatile register before it is saved is data",
              "[codegen][gapfill]") {
  const auto tree = Generate({kLiR3One, kBlr, kMrR31R3, kAddiR3FromR31, kBlr});
  CHECK_FALSE(HasFunction(tree, 2));
}

REX_TEST_CASE("A store of a nonvolatile register through another base is data",
              "[codegen][gapfill]") {
  const auto tree = Generate({kLiR3One, kBlr, kStwR31ToR3, kBlr});
  CHECK_FALSE(HasFunction(tree, 2));
}

REX_TEST_CASE("A store of a nonvolatile register through the stack pointer is a function",
              "[codegen][gapfill]") {
  const auto tree = Generate({kLiR3One, kBlr, kStwR31ToR1, kLiR3One, kBlr});
  CHECK(HasFunction(tree, 2));
}

REX_TEST_CASE("A trap stub that follows another function is a function", "[codegen][gapfill]") {
  const auto tree = Generate({kLiR3One, kBlr, kTwiAlways, 0, kLiR3Two, kBlr});
  CHECK(HasFunction(tree, 2));
  CHECK(HasFunction(tree, 4));
}

REX_TEST_CASE("A lone return after a count register thunk is its own function",
              "[codegen][gapfill]") {
  const auto tree =
      Generate({kLiR3One, kBlr, kLwzR11FromR3, kLwzR10From12R11, kMtctrR10, kBctr, kBlr});
  CHECK(HasFunction(tree, 2));
  CHECK(HasFunction(tree, 6));
}

namespace {

constexpr u32 kMtctrR11 = 0x7D6903A6;
constexpr u32 kImportFunctionRecord = 1u << 24;

std::string GenerateWithThunks(const std::vector<u32>& words, std::vector<u32> records) {
  rex::test::TempFolder folder;
  const rex::test::PDataEntry pdata[] = {{Address(0), 2}};
  const auto result = rex::test::GenerateFromImage(
      folder, rex::test::BuildCodeImage(words, pdata), {}, true,
      {rex::test::XEXImportSpec{"xboxkrnl.exe", std::move(records), 0}});
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  return rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
}

}  // namespace

REX_TEST_CASE("Back to back import thunks leave no function inside either thunk",
              "[codegen][gapfill]") {
  constexpr u32 kFirst = 4;
  constexpr u32 kSecond = 8;
  const std::vector<u32> shapes[] = {{0, kMtctrR11, kBctr}, {kLiR3Zero, kMtctrR11, kBctr}};
  for (const auto& shape : shapes) {
    std::vector<u32> words = {kLiR3One, kBlr, 0, 0};
    for (u32 thunk : {kFirst, kSecond}) {
      words.push_back(kImportFunctionRecord | (0x7FF0 + thunk));
      words.insert(words.end(), shape.begin(), shape.end());
    }
    const auto tree = GenerateWithThunks(words, {Address(kFirst), Address(kSecond)});
    for (u32 thunk : {kFirst, kSecond}) {
      for (u32 inside = 1; inside < 4; ++inside)
        CHECK_FALSE(HasFunction(tree, thunk + inside));
    }
  }
}

namespace {

constexpr u32 kCmplwiR3Zero = 0x28030000;
constexpr u32 kBeqlr = 0x4D820020;
constexpr u32 kLisR11 = 0x3D600001;
constexpr u32 kLwzR11 = 0x816B0000;

bool Calls(const std::string& tree, u32 index) {
  return tree.find(std::format("sub_{:08X}(ctx, base);", Address(index))) != std::string::npos;
}

}  // namespace

REX_TEST_CASE("A body that ends in a conditional return continues into the function after it",
              "[codegen][gapfill]") {
  const auto tree =
      Generate({kLwzR3, kCmplwiR3Zero, kBeqlr, kLisR11, kLwzR11, 0x7D6903A6, kBctr}, 3);
  CHECK(HasFunction(tree, 3));
  CHECK(Calls(tree, 3));
}

REX_TEST_CASE(
    "A body that ends in a call continues into the function after it when the callee returns",
    "[codegen][gapfill]") {
  const auto tree = Generate({Branch(0, 4) | 1, kLiR3One, kBlr, 0, kLiR3Two, kBlr}, 1);
  CHECK(HasFunction(tree, 1));
  CHECK(Calls(tree, 1));
}

REX_TEST_CASE("A body that ends in a call that never returns does not continue",
              "[codegen][gapfill]") {
  const auto tree = Generate({Branch(0, 4) | 1, kLiR3One, kBlr, 0, kTwiAlways, 0}, 1);
  CHECK(HasFunction(tree, 1));
  CHECK_FALSE(Calls(tree, 1));
}

REX_TEST_CASE("A jump table case past the declared end runs to its own branch",
              "[codegen][gapfill]") {
  constexpr u32 kTable = 6;
  constexpr u32 kFirstCase = 8;
  constexpr u32 kLastCase = 10;
  constexpr u32 kTailTarget = 14;
  const u32 table = Address(kTable);
  const u32 hi = (table + 0x8000) >> 16;
  const u32 lo = table & 0xFFFF;
  const auto tree = Generate({0x3D600000 | hi, 0x396B0000 | lo, kSlwiR10R3Two, kLwzxR10R11R10,
                              kMtctrR10, kBctr, Address(kFirstCase), Address(kLastCase), kLiR3One,
                              kBlr, kLiR3Two, Branch(11, kTailTarget), 0, 0, kLiR3Zero, kBlr},
                             kLastCase);
  CHECK_FALSE(HasFunction(tree, kLastCase + 1));
  CHECK(Calls(tree, kTailTarget));
}

REX_TEST_CASE("A jump table case past the declared end stops at the next function",
              "[codegen][gapfill]") {
  constexpr u32 kTable = 6;
  constexpr u32 kFirstCase = 8;
  constexpr u32 kLastCase = 10;
  constexpr u32 kNextFunction = 11;
  const u32 table = Address(kTable);
  const u32 hi = (table + 0x8000) >> 16;
  const u32 lo = table & 0xFFFF;
  const std::vector<u32> words = {0x3D600000 | hi,
                                  0x396B0000 | lo,
                                  kSlwiR10R3Two,
                                  kLwzxR10R11R10,
                                  kMtctrR10,
                                  kBctr,
                                  Address(kFirstCase),
                                  Address(kLastCase),
                                  kLiR3One,
                                  kBlr,
                                  kLiR3Two,
                                  kLiR3Three,
                                  kBlr};
  const rex::test::PDataEntry pdata[] = {{Address(0), kLastCase}, {Address(kNextFunction), 2}};
  rex::test::TempFolder folder;
  const auto result = rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(words, pdata));
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  const auto tree = rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
  CHECK(Calls(tree, kNextFunction));
  size_t copies = 0;
  for (size_t at = tree.find("// li r3,3"); at != std::string::npos;
       at = tree.find("// li r3,3", at + 1)) {
    ++copies;
  }
  CHECK(copies == 1);
}

namespace {

constexpr u32 kBneCR6 = 0x409A0000;
constexpr u32 kAddiR3One = 0x38630001;
constexpr u32 kCmpwiR3Nine = 0x2C030009;
constexpr u32 kBltBack4 = 0x4180FFF0;

}  // namespace

REX_TEST_CASE("A branch that jumps over a return is a tail call to the function after it",
              "[codegen][gapfill]") {
  const auto tree = Generate({kLiR3One, kBlr, kCmplwiCR6R3Zero, kBneCR6 | 8, Branch(4, 7), kLiR3One,
                              kBlr, kLiR3Two, kBlr});
  CHECK(HasFunction(tree, 2));
  CHECK(HasFunction(tree, 7));
  CHECK_FALSE(HasFunction(tree, 5));
}

REX_TEST_CASE("A loop inverted entry stays 1 function", "[codegen][gapfill]") {
  const auto tree = Generate({kLiR3One, kBlr, Branch(2, 6), kAddiR3One, kAddiR3One, kAddiR3One,
                              kCmpwiR3Nine, kBltBack4, kBlr});
  CHECK(HasFunction(tree, 2));
  for (u32 index = 3; index < 9; ++index)
    CHECK_FALSE(HasFunction(tree, index));
}
