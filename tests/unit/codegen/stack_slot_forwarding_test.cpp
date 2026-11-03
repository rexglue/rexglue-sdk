/**
 * @file        tests/unit/codegen/stack_slot_forwarding_test.cpp
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

#include <string>
#include <string_view>
#include <vector>

namespace {

using rex::u32;

constexpr u32 kBlr = 0x4E800020;
constexpr u32 kNop = 0x60000000;
constexpr u32 kStwR11Slot50 = 0x91610050;
constexpr u32 kStwR12Slot54 = 0x91810054;
constexpr u32 kStwR12ThroughR3 = 0x91830000;
constexpr u32 kStbR12Slot51 = 0x99810051;
constexpr u32 kStwR11Slot50ThroughR3 = 0x91630050;
constexpr u32 kFloatWriteF1Slot50 = 0xD0210050;
constexpr u32 kLisR11 = 0x3D601234;
constexpr u32 kLwzR3Slot50 = 0x80610050;
constexpr u32 kLwzR4Slot54 = 0x80810054;
constexpr u32 kLwzR4Slot50ThroughR3 = 0x80830050;
constexpr u32 kLhzR3Slot50 = 0xA0610050;
constexpr u32 kLfsF0Slot50 = 0xC0010050;
constexpr u32 kLfsF2Slot50 = 0xC0410050;
constexpr u32 kAddiR1 = 0x38210080;
constexpr u32 kBeqSkipOne = 0x41820008;
constexpr u32 kBlSkipTwo = 0x4800000D;
constexpr u32 kStwR11Slot0 = 0x91610000;
constexpr u32 kLfsF1FromR3 = 0xC0230000;
constexpr u32 kCmpwCR1 = 0x7C832000;
constexpr u32 kVaddfpV1 = 0x1020180A;
constexpr u32 kSlotCount = 9;

std::string Generate(const std::vector<u32>& words, std::string_view flags) {
  const rex::test::PDataEntry pdata[] = {
      {rex::test::kCodeImageBase + rex::test::kCodeImageCodeOffset,
       static_cast<u32>(words.size())}};
  rex::test::TempFolder folder;
  const auto result =
      rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(words, pdata), flags);
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  const auto tree = rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
  const auto begin = tree.find("DEFINE_REX_FUNC(xstart)");
  REQUIRE(begin != std::string::npos);
  return tree.substr(begin, tree.find("\n}\n", begin) - begin);
}

std::string Forwarded(const std::vector<u32>& words) {
  return Generate(words, "forward_stack_stores = true\n");
}

bool Has(const std::string& tree, std::string_view text) {
  return tree.find(text) != std::string::npos;
}

}  // namespace

REX_TEST_CASE("A load from a written stack slot reads the held value", "[codegen][forwarding]") {
  const auto tree = Forwarded({kStwR11Slot50, kLisR11, kLwzR3Slot50, kBlr});
  CHECK(Has(tree, "PPCRegister fwd0{}"));
  CHECK(Has(tree, "fwd0.u32 = "));
  CHECK(Has(tree, " = fwd0.u32;"));
  CHECK_FALSE(Has(tree, "LOAD_U32"));
}

REX_TEST_CASE("Forwarding is off without the manifest key", "[codegen][forwarding]") {
  const auto tree = Generate({kStwR11Slot50, kLisR11, kLwzR3Slot50, kBlr}, "");
  CHECK_FALSE(Has(tree, "fwd0"));
  CHECK(Has(tree, "REX_STACK_LOAD_U32"));
}

REX_TEST_CASE("Two slots forward independently", "[codegen][forwarding]") {
  const auto tree = Forwarded({kStwR11Slot50, kStwR12Slot54, kLwzR4Slot54, kLwzR3Slot50, kBlr});
  CHECK(Has(tree, "PPCRegister fwd1{}"));
  CHECK(Has(tree, " = fwd0.u32;"));
  CHECK(Has(tree, " = fwd1.u32;"));
  CHECK_FALSE(Has(tree, "LOAD_U32"));
}

REX_TEST_CASE("A label ends forwarding", "[codegen][forwarding]") {
  const auto tree = Forwarded({kStwR11Slot50, kBeqSkipOne, kNop, kLwzR3Slot50, kBlr});
  CHECK(Has(tree, "REX_STACK_LOAD_U32"));
  CHECK_FALSE(Has(tree, " = fwd0.u32;"));
}

REX_TEST_CASE("A call ends forwarding", "[codegen][forwarding]") {
  const auto tree = Forwarded({kStwR11Slot50, kBlSkipTwo, kLwzR3Slot50, kBlr, kBlr});
  CHECK(Has(tree, "REX_STACK_LOAD_U32"));
  CHECK_FALSE(Has(tree, " = fwd0.u32;"));
}

REX_TEST_CASE("A float load into f1 keeps the forwarded slot", "[codegen][forwarding]") {
  const auto tree = Forwarded({kStwR11Slot50, kLfsF1FromR3, kLwzR3Slot50, kBlr});
  CHECK(Has(tree, " = fwd0.u32;"));
}

REX_TEST_CASE("A compare into cr1 keeps the forwarded slot", "[codegen][forwarding]") {
  const auto tree = Forwarded({kStwR11Slot50, kCmpwCR1, kLwzR3Slot50, kBlr});
  CHECK(Has(tree, " = fwd0.u32;"));
}

REX_TEST_CASE("A vector write into v1 keeps the forwarded slot", "[codegen][forwarding]") {
  const auto tree = Forwarded({kStwR11Slot50, kVaddfpV1, kLwzR3Slot50, kBlr});
  CHECK(Has(tree, " = fwd0.u32;"));
}

REX_TEST_CASE("Moving r1 ends forwarding", "[codegen][forwarding]") {
  const auto tree = Forwarded({kStwR11Slot50, kAddiR1, kLwzR3Slot50, kBlr});
  CHECK(Has(tree, "REX_STACK_LOAD_U32"));
  CHECK_FALSE(Has(tree, " = fwd0.u32;"));
}

REX_TEST_CASE("A write through another base ends forwarding", "[codegen][forwarding]") {
  const auto tree = Forwarded({kStwR11Slot50, kStwR12ThroughR3, kLwzR3Slot50, kBlr});
  CHECK(Has(tree, "REX_STACK_LOAD_U32"));
  CHECK_FALSE(Has(tree, " = fwd0.u32;"));
}

REX_TEST_CASE("A narrower write inside the slot ends forwarding", "[codegen][forwarding]") {
  const auto tree = Forwarded({kStwR11Slot50, kStbR12Slot51, kLwzR3Slot50, kBlr});
  CHECK(Has(tree, "REX_STACK_LOAD_U32"));
  CHECK_FALSE(Has(tree, "fwd0.u32;"));
}

REX_TEST_CASE("A load of another width reads memory", "[codegen][forwarding]") {
  const auto tree = Forwarded({kStwR11Slot50, kLhzR3Slot50, kBlr});
  CHECK(Has(tree, "REX_STACK_LOAD_U16"));
  CHECK_FALSE(Has(tree, " = fwd0.u16;"));
}

REX_TEST_CASE("Only r1 slots forward", "[codegen][forwarding]") {
  const auto tree = Forwarded({kStwR11Slot50ThroughR3, kLwzR4Slot50ThroughR3, kBlr});
  CHECK_FALSE(Has(tree, "fwd0"));
  CHECK(Has(tree, "REX_LOAD_U32"));
}

REX_TEST_CASE("A float write forwards its single precision pattern", "[codegen][forwarding]") {
  const auto tree = Forwarded({kFloatWriteF1Slot50, kLfsF2Slot50, kBlr});
  CHECK(Has(tree, "fwd0.f32 = f32("));
  CHECK(Has(tree, "= f64(fwd0.f32);"));
  CHECK_FALSE(Has(tree, "LOAD_U32"));
}

REX_TEST_CASE("A word write forwards to a float load", "[codegen][forwarding]") {
  const auto tree = Forwarded({kStwR11Slot50, kLfsF0Slot50, kBlr});
  CHECK(Has(tree, "= f64(fwd0.f32);"));
  CHECK_FALSE(Has(tree, "LOAD_U32"));
}

REX_TEST_CASE("The ninth slot evicts the first", "[codegen][forwarding]") {
  std::vector<u32> words;
  for (u32 i = 0; i < kSlotCount; ++i) {
    words.push_back(kStwR11Slot0 | (0x50 + 4 * i));
  }
  words.push_back(kLwzR3Slot50);
  words.push_back(kBlr);
  const auto tree = Forwarded(words);
  CHECK(Has(tree, "REX_STACK_LOAD_U32"));
}
