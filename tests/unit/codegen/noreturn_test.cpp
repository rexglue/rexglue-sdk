/**
 * @file        tests/unit/codegen/noreturn_test.cpp
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
constexpr u32 kBeqlr = 0x4D820020;
constexpr u32 kBranchAlwaysToSelf = 0x42800000;
constexpr u32 kLiR3Zero = 0x38600000;
constexpr u32 kLiR3One = 0x38600001;
constexpr u32 kAddiR3One = 0x38630001;
constexpr u32 kMtctrR11 = 0x7D6903A6;
constexpr u32 kBctr = 0x4E800420;
constexpr u32 kImportFunctionRecord = 1u << 24;
constexpr u32 kExTerminateThread = 0x19;
constexpr u32 kKeDelayExecutionThread = 0x5A;
constexpr u32 kFunctionWords = 2;

constexpr u32 Address(u32 index) {
  return rex::test::kCodeImageBase + rex::test::kCodeImageCodeOffset + index * 4;
}

constexpr u32 BranchLink(u32 from_index, u32 to_index) {
  return 0x48000001 |
         ((static_cast<i32>(to_index) - static_cast<i32>(from_index)) * 4 & 0x03FFFFFC);
}

struct Image {
  std::vector<u32> words;
  std::vector<rex::test::PDataEntry> pdata;
  u32 thunk = 0;
};

void AddFunction(Image& image, u32 second_word, u32 first_word = kLiR3One) {
  const u32 index = static_cast<u32>(image.words.size());
  image.pdata.push_back({Address(index), kFunctionWords});
  image.words.push_back(first_word);
  image.words.push_back(second_word);
}

struct Output {
  std::string tree;
  size_t run_offs = 0;
};

Output Run(Image image, u32 ordinal, std::string_view manifest, bool count_run_offs) {
  image.thunk = static_cast<u32>(image.words.size());
  image.words.push_back(kImportFunctionRecord | ordinal);
  image.words.insert(image.words.end(), {kLiR3Zero, kMtctrR11, kBctr});
  rex::log::CaptureSink sink;
  rex::test::TempFolder folder;
  const auto result = rex::test::GenerateFromImage(
      folder, rex::test::BuildCodeImage(image.words, image.pdata), manifest, true,
      {rex::test::XEXImportSpec{"xboxkrnl.exe", {Address(image.thunk)}, 0}},
      count_run_offs ? &sink : nullptr);
  Output output;
  if (count_run_offs) {
    std::vector<rex::log::CapturedMessage> messages;
    sink.CopySince(0, messages);
    output.run_offs = static_cast<size_t>(
        std::ranges::count_if(messages, [](const rex::log::CapturedMessage& message) {
          return message.text.contains("runs off its end");
        }));
  }
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  output.tree = rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
  return output;
}

std::string Generate(Image image, u32 ordinal, std::string_view manifest = {}) {
  return Run(std::move(image), ordinal, manifest, false).tree;
}

bool Calls(const std::string& tree, u32 index) {
  return tree.find(std::format("sub_{:08X}(ctx, base);", Address(index))) != std::string::npos;
}

Image CallerOverFollower(u32 callee_tail) {
  Image image;
  AddFunction(image, BranchLink(1, 4));
  AddFunction(image, kBlr);
  AddFunction(image, callee_tail);
  AddFunction(image, kBlr);
  return image;
}

}  // namespace

REX_TEST_CASE("A function that ends in a call to a no return import does not fall into the next",
              "[codegen][noreturn]") {
  const auto tree = Generate(CallerOverFollower(BranchLink(5, 8)), kExTerminateThread);
  CHECK_FALSE(Calls(tree, 2));
  CHECK_FALSE(Calls(tree, 6));
}

REX_TEST_CASE("A function that ends in a call to a returning import falls into the next",
              "[codegen][noreturn]") {
  const auto tree = Generate(CallerOverFollower(BranchLink(5, 8)), kKeDelayExecutionThread);
  CHECK(Calls(tree, 2));
  CHECK(Calls(tree, 6));
}

REX_TEST_CASE("A call to a function that never returns does not fall into the next",
              "[codegen][noreturn]") {
  Image image;
  AddFunction(image, BranchLink(1, 4));
  AddFunction(image, kBlr);
  AddFunction(image, BranchLink(5, 8));
  AddFunction(image, kBlr);
  AddFunction(image, BranchLink(9, 12));
  AddFunction(image, kBlr);
  const auto tree = Generate(std::move(image), kExTerminateThread);
  CHECK_FALSE(Calls(tree, 2));
  CHECK_FALSE(Calls(tree, 6));
  CHECK_FALSE(Calls(tree, 10));
}

REX_TEST_CASE("A function with a return on one path keeps falling into the next",
              "[codegen][noreturn]") {
  Image image;
  AddFunction(image, BranchLink(1, 4));
  AddFunction(image, kBlr);
  AddFunction(image, BranchLink(5, 8), kBeqlr);
  AddFunction(image, kBlr);
  const auto tree = Generate(std::move(image), kExTerminateThread);
  CHECK(Calls(tree, 2));
  CHECK_FALSE(Calls(tree, 6));
}

REX_TEST_CASE("A call to the longjmp routine does not fall into the next", "[codegen][noreturn]") {
  Image image;
  AddFunction(image, BranchLink(1, 4));
  AddFunction(image, kBlr);
  AddFunction(image, kBlr);
  const auto tree = Generate(std::move(image), kKeDelayExecutionThread,
                             std::format("longjmp_address = 0x{:08X}\n", Address(4)));
  CHECK_FALSE(Calls(tree, 2));
}

REX_TEST_CASE("A body ending in a call followed by padding does not warn", "[codegen][noreturn]") {
  Image image;
  AddFunction(image, BranchLink(1, 5));
  image.words.push_back(0);
  AddFunction(image, kBlr);
  AddFunction(image, kBlr);
  const auto output = Run(std::move(image), kKeDelayExecutionThread, {}, true);
  CHECK(output.run_offs == 0);
}

REX_TEST_CASE("A body ending in an add followed by padding warns", "[codegen][noreturn]") {
  Image image;
  AddFunction(image, kAddiR3One);
  image.words.push_back(0);
  AddFunction(image, kBlr);
  const auto output = Run(std::move(image), kKeDelayExecutionThread, {}, true);
  CHECK(output.run_offs == 1);
}

REX_TEST_CASE("A function whose every path ends in a call with no return path never returns",
              "[codegen][noreturn]") {
  Image image;
  AddFunction(image, BranchLink(1, 4));
  AddFunction(image, kBlr);
  AddFunction(image, BranchLink(5, 7));
  image.words.push_back(0);
  AddFunction(image, kBlr);
  AddFunction(image, BranchLink(10, 7));
  AddFunction(image, kBlr);
  const auto tree = Generate(std::move(image), kKeDelayExecutionThread);
  CHECK_FALSE(Calls(tree, 2));
  CHECK(Calls(tree, 11));
}

REX_TEST_CASE("A conditional branch whose condition is ignored ends the path like a branch",
              "[codegen][noreturn]") {
  Image image;
  AddFunction(image, BranchLink(1, 4));
  AddFunction(image, kBlr);
  AddFunction(image, kBranchAlwaysToSelf);
  AddFunction(image, kBlr);
  const auto tree = Generate(std::move(image), kKeDelayExecutionThread);
  CHECK_FALSE(Calls(tree, 2));
  CHECK_FALSE(Calls(tree, 6));
}
