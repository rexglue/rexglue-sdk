/**
 * @file        tests/unit/codegen/millicode_test.cpp
 * @brief       Save and restore runs register only when every entry is valid
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
#include <vector>

namespace {

using rex::i32;
using rex::u32;

constexpr u32 kBlr = 0x4E800020;
constexpr u32 kMtlrR12 = 0x7D8803A6;
constexpr u32 kOriR3 = 0x60630001;
constexpr u32 kMrR12R3 = 0x7C6C1B78;
constexpr u32 kHelperIndex = 16;
constexpr u32 kGPRCount = 18;
constexpr u32 kVMXExtendedCount = 64;

constexpr u32 DForm(u32 opcode, u32 rt, u32 ra, i32 disp) {
  return opcode << 26 | rt << 21 | ra << 16 | (static_cast<u32>(disp) & 0xFFFF);
}

constexpr u32 Address(u32 index) {
  return rex::test::kCodeImageBase + rex::test::kCodeImageCodeOffset + index * 4;
}

constexpr u32 BranchLink(u32 from_index, u32 to_index) {
  return 0x48000001 |
         ((static_cast<i32>(to_index) - static_cast<i32>(from_index)) * 4 & 0x03FFFFFC);
}

constexpr u32 Branch(u32 from_index, u32 to_index) {
  return 0x48000000 |
         ((static_cast<i32>(to_index) - static_cast<i32>(from_index)) * 4 & 0x03FFFFFC);
}

constexpr u32 VMXWord(bool save, u32 reg) {
  const u32 base = save ? 0x7C0001CE : 0x7C0000CE;
  return base | reg << 21 | 11 << 16 | 12 << 11;
}

constexpr u32 VMX128Word(bool save, u32 reg) {
  const u32 base = save ? 0x100B61CB : 0x100B60CB;
  const u32 index = reg - 64;
  return base + (index % 32) * 0x00200000 + (index >= 32 ? 4 : 0);
}

enum class Run { kGPRSave, kGPRRestore, kFPRSave, kFPRRestore, kVMXSave, kVMXRestore, kVMXXSave };

struct RunSpec {
  Run run = Run::kGPRSave;
  u32 first = 14;
  u32 count = kGPRCount;
  bool closed = true;
  bool foreign = false;
};

std::vector<u32> BuildRun(const RunSpec& spec) {
  std::vector<u32> words;
  const bool save = spec.run == Run::kGPRSave || spec.run == Run::kFPRSave ||
                    spec.run == Run::kVMXSave || spec.run == Run::kVMXXSave;
  for (u32 i = 0; i < spec.count; ++i) {
    const u32 reg = spec.first + i;
    switch (spec.run) {
      case Run::kGPRSave:
      case Run::kGPRRestore:
        words.push_back(DForm(save ? 62 : 58, reg, 1, -8 * static_cast<i32>(33 - reg)));
        break;
      case Run::kFPRSave:
      case Run::kFPRRestore:
        words.push_back(DForm(save ? 54 : 50, reg, 12, -8 * static_cast<i32>(32 - reg)));
        break;
      case Run::kVMXSave:
      case Run::kVMXRestore:
        words.push_back(DForm(14, 11, 0, -16 * static_cast<i32>(32 - reg)));
        words.push_back(VMXWord(save, reg));
        break;
      case Run::kVMXXSave:
        words.push_back(DForm(14, 11, 0, -16 * static_cast<i32>(128 - reg)));
        words.push_back(VMX128Word(true, reg));
        break;
    }
  }
  if (spec.foreign && words.size() > 3)
    words[2] = kOriR3;
  if (spec.run == Run::kGPRSave)
    words.push_back(DForm(36, 12, 1, -8));
  if (spec.run == Run::kGPRRestore) {
    words.push_back(DForm(32, 12, 1, -8));
    words.push_back(kMtlrR12);
  }
  if (spec.closed)
    words.push_back(kBlr);
  return words;
}

std::string Generate(const std::vector<u32>& code, std::string_view extra_manifest = {},
                     u32 pdata_length = 2) {
  rex::test::TempFolder folder;
  const rex::test::PDataEntry pdata[] = {{Address(0), pdata_length}};
  const auto result =
      rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(code, pdata), extra_manifest);
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  return rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
}

std::string GenerateError(const std::vector<u32>& code, std::string_view extra_manifest = {}) {
  rex::test::TempFolder folder;
  const rex::test::PDataEntry pdata[] = {{Address(0), 2}};
  const auto result = rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(code, pdata),
                                                   extra_manifest, false);
  REQUIRE_FALSE(result);
  return result.error().message;
}

std::vector<u32> WithRun(const RunSpec& spec, std::vector<u32> leading) {
  leading.resize(kHelperIndex, 0);
  const auto run = BuildRun(spec);
  leading.insert(leading.end(), run.begin(), run.end());
  return leading;
}

std::vector<u32> WithRun(const RunSpec& spec) {
  return WithRun(spec, {kBlr, kBlr});
}

bool Has(const std::string& tree, const std::string& text) {
  return tree.find(text) != std::string::npos;
}

std::string Body(const std::string& tree, std::string_view name) {
  const auto begin = tree.find(std::format("DEFINE_REX_FUNC({})", name));
  REQUIRE(begin != std::string::npos);
  return tree.substr(begin, tree.find("\n}\n", begin) - begin);
}

std::string Entry(u32 index, std::string_view name) {
  return std::format("{{ 0x{:08X}, {} }}", Address(index), name);
}

}  // namespace

REX_TEST_CASE("A full GPR run is registered from r14", "[codegen][millicode]") {
  const auto tree = Generate(WithRun({Run::kGPRSave}));
  CHECK(Has(tree, Entry(kHelperIndex, "__savegprlr_14")));
  CHECK(Has(tree, Entry(kHelperIndex + kGPRCount - 1, "__savegprlr_31")));
  CHECK_FALSE(Has(tree, "__savegprlr_13"));
}

REX_TEST_CASE("A GPR run with an r13 entry is registered from r13", "[codegen][millicode]") {
  const auto tree = Generate(WithRun({Run::kGPRRestore, 13, kGPRCount + 1}));
  CHECK(Has(tree, Entry(kHelperIndex, "__restgprlr_13")));
  CHECK(Has(tree, Entry(kHelperIndex + 1, "__restgprlr_14")));
  CHECK(Has(tree, Entry(kHelperIndex + kGPRCount, "__restgprlr_31")));
}

REX_TEST_CASE("Both ends of an FPR run register", "[codegen][millicode]") {
  const auto tree = Generate(WithRun({Run::kFPRSave}));
  CHECK(Has(tree, Entry(kHelperIndex, "__savefpr_14")));
  CHECK(Has(tree, Entry(kHelperIndex + kGPRCount - 1, "__savefpr_31")));
}

REX_TEST_CASE("Both ends of a VMX run register", "[codegen][millicode]") {
  const auto tree = Generate(WithRun({Run::kVMXRestore}));
  CHECK(Has(tree, Entry(kHelperIndex, "__restvmx_14")));
  CHECK(Has(tree, Entry(kHelperIndex + 2 * (kGPRCount - 1), "__restvmx_31")));
}

REX_TEST_CASE("Both ends of an extended VMX run register", "[codegen][millicode]") {
  const auto tree = Generate(WithRun({Run::kVMXXSave, 64, kVMXExtendedCount}));
  CHECK(Has(tree, Entry(kHelperIndex, "__savevmx_64")));
  CHECK(Has(tree, Entry(kHelperIndex + 2 * (kVMXExtendedCount - 1), "__savevmx_127")));
}

REX_TEST_CASE("An inlined register save before the real run is not taken for a helper",
              "[codegen][millicode]") {
  auto code =
      WithRun({Run::kGPRSave}, {kBlr, DForm(62, 14, 1, -152), DForm(62, 15, 1, -144), kBlr});
  const auto tree = Generate(code);
  CHECK(Has(tree, Entry(kHelperIndex, "__savegprlr_14")));
  CHECK_FALSE(Has(tree, Entry(1, "__savegprlr_14")));
}

REX_TEST_CASE("A run without its closing return is rejected", "[codegen][millicode]") {
  RunSpec spec{Run::kGPRSave};
  spec.closed = false;
  CHECK_FALSE(Has(Generate(WithRun(spec)), "__savegprlr_14"));
}

REX_TEST_CASE("A run with a foreign instruction is rejected", "[codegen][millicode]") {
  RunSpec spec{Run::kGPRSave};
  spec.foreign = true;
  CHECK_FALSE(Has(Generate(WithRun(spec)), "__savegprlr_14"));
}

REX_TEST_CASE("A run shorter than the kind requires is rejected", "[codegen][millicode]") {
  CHECK_FALSE(Has(Generate(WithRun({Run::kGPRSave, 14, kGPRCount - 1})), "__savegprlr_14"));
}

REX_TEST_CASE("A save helper call is dropped in a function that localizes",
              "[codegen][millicode]") {
  auto code = WithRun({Run::kGPRSave}, {BranchLink(0, kHelperIndex), kBlr});
  const auto tree = Generate(code, "non_volatile_as_local = true\n");
  CHECK_FALSE(Has(tree, "__savegprlr_14(ctx, base)"));
}

REX_TEST_CASE("A save helper call is kept in a function that does not localize",
              "[codegen][millicode]") {
  auto code = WithRun({Run::kGPRSave}, {BranchLink(0, kHelperIndex), kBlr});
  const auto tree = Generate(code);
  CHECK(Has(tree, "__savegprlr_14(ctx, base)"));
}

REX_TEST_CASE("A save helper call is kept in a function that keeps registers in ctx",
              "[codegen][millicode]") {
  auto code = WithRun({Run::kGPRSave}, {BranchLink(0, kHelperIndex), kBlr});
  const auto tree =
      Generate(code, std::format("non_volatile_as_local = true\n[entrypoint.functions]\n"
                                 "0x{:08X} = {{ share_registers = true }}\n",
                                 Address(0)));
  CHECK(Has(tree, "__savegprlr_14(ctx, base)"));
}

REX_TEST_CASE("A helper body keeps its registers in ctx under non_volatile_as_local",
              "[codegen][millicode]") {
  struct Case {
    Run run;
    u32 first;
    u32 count;
    std::string_view name;
    std::string_view reg;
  };
  const Case cases[] = {
      {Run::kGPRSave, 14, kGPRCount, "__savegprlr_29", "r29"},
      {Run::kGPRRestore, 14, kGPRCount, "__restgprlr_29", "r29"},
      {Run::kFPRSave, 14, kGPRCount, "__savefpr_29", "f29"},
      {Run::kFPRRestore, 14, kGPRCount, "__restfpr_29", "f29"},
      {Run::kVMXSave, 14, kGPRCount, "__savevmx_29", "v29"},
      {Run::kVMXRestore, 14, kGPRCount, "__restvmx_29", "v29"},
      {Run::kVMXXSave, 64, kVMXExtendedCount, "__savevmx_100", "v100"},
  };
  for (const auto& c : cases) {
    const auto tree =
        Generate(WithRun({c.run, c.first, c.count}), "non_volatile_as_local = true\n");
    const auto body = Body(tree, c.name);
    INFO(c.name);
    CHECK(Has(body, std::format("ctx.{}", c.reg)));
    CHECK_FALSE(Has(body, std::format("{}{{}}", c.reg)));
  }
}

REX_TEST_CASE("A function that keeps registers in ctx returns with the saved register intact",
              "[codegen][millicode]") {
  constexpr u32 kSaveLength = kGPRCount + 2;
  auto code = WithRun({Run::kGPRSave}, {BranchLink(0, kHelperIndex), DForm(14, 31, 0, 5),
                                        BranchLink(2, kHelperIndex + kSaveLength), kBlr});
  const auto restore = BuildRun({Run::kGPRRestore});
  code.insert(code.end(), restore.begin(), restore.end());
  const auto tree = Generate(code,
                             std::format("non_volatile_as_local = true\n[entrypoint.functions]\n"
                                         "0x{:08X} = {{ share_registers = true }}\n",
                                         Address(0)),
                             4);
  const auto save_call = tree.find("__savegprlr_14(ctx, base)");
  const auto write = tree.find("ctx.r31.s64 = 5;");
  const auto restore_call = tree.find("__restgprlr_14(ctx, base)");
  REQUIRE(save_call != std::string::npos);
  REQUIRE(write != std::string::npos);
  REQUIRE(restore_call != std::string::npos);
  CHECK(save_call < write);
  CHECK(write < restore_call);
  CHECK(Has(Body(tree, "__savegprlr_31"), "ctx.r31.u64"));
  CHECK(Has(Body(tree, "__restgprlr_31"), "ctx.r31.u64"));
}

REX_TEST_CASE("A config function named like a helper keeps its calls", "[codegen][millicode]") {
  std::vector<u32> code = {BranchLink(0, 2), kBlr, kMrR12R3, kBlr};
  const auto tree =
      Generate(code, std::format("non_volatile_as_local = true\n[entrypoint.functions]\n"
                                 "0x{:08X} = {{ name = \"__savegprlr_99\" }}\n",
                                 Address(2)));
  CHECK(Has(tree, "__savegprlr_99(ctx, base)"));
}

REX_TEST_CASE("A helper call passes the caller's local r12", "[codegen][millicode]") {
  auto code = WithRun({Run::kGPRSave}, {kMrR12R3, BranchLink(1, kHelperIndex), kBlr});
  const auto tree = Generate(code, "non_argument_as_local = true\n", 3);
  const auto sync = tree.find("ctx.r12 = r12;");
  const auto call = tree.find("__savegprlr_14(ctx, base)");
  CHECK(sync != std::string::npos);
  CHECK(call != std::string::npos);
  CHECK(sync < call);
}

REX_TEST_CASE("A branch into the middle of a run is a codegen error", "[codegen][millicode]") {
  const auto message = GenerateError(WithRun({Run::kVMXSave}, {Branch(0, kHelperIndex + 1), kBlr}));
  CHECK(Has(message, "millicode"));
}

REX_TEST_CASE("A config entry at a helper address is a codegen error", "[codegen][millicode]") {
  const auto message =
      GenerateError(WithRun({Run::kGPRSave}),
                    std::format("[entrypoint.functions]\n0x{:08X} = {{ name = \"mine\" }}\n",
                                Address(kHelperIndex)));
  CHECK(Has(message, "millicode"));
}

REX_TEST_CASE("An SEH function that saves GPR then FPR restores through both",
              "[codegen][millicode]") {
  constexpr u32 kMflrR12 = 0x7D8802A6;
  constexpr u32 kGPRSaveIndex = 16;
  constexpr u32 kFPRSaveIndex = 57;
  std::vector<u32> code = {Address(9),
                           rex::test::kCodeImageBase + rex::test::kCodeImageRDataOffset,
                           kMflrR12,
                           BranchLink(3, kGPRSaveIndex),
                           BranchLink(4, kFPRSaveIndex),
                           DForm(14, 31, 1, -96),
                           kBlr,
                           0,
                           kBlr,
                           kBlr};
  code.resize(kGPRSaveIndex, 0);
  for (const RunSpec& spec : {RunSpec{Run::kGPRSave}, RunSpec{Run::kGPRRestore},
                              RunSpec{Run::kFPRSave}, RunSpec{Run::kFPRRestore}}) {
    const auto run = BuildRun(spec);
    code.insert(code.end(), run.begin(), run.end());
  }
  const std::vector<u32> rdata = {1, Address(2), Address(5), Address(8), 0};
  const rex::test::PDataEntry pdata[] = {{Address(2), 5, 4, rex::test::kPDataHandler}};

  rex::test::TempFolder folder;
  const auto result =
      rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(code, pdata, rdata),
                                   "generate_exception_handlers = true\n");
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  const auto tree = rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
  CHECK(Has(tree, "__restgprlr_14(ctx, base);  // Restore caller registers"));
  CHECK(Has(tree, "__restfpr_14(ctx, base);  // Restore caller registers"));
}

REX_TEST_CASE("A pdata type 0 head confirms a save run", "[codegen][millicode]") {
  rex::test::TempFolder folder;
  const rex::test::PDataEntry pdata[] = {
      {Address(0), 2}, {Address(kHelperIndex), 20, 0, rex::test::kPDataSaveMillicode}};
  const auto result = rex::test::GenerateFromImage(
      folder, rex::test::BuildCodeImage(WithRun({Run::kGPRSave}, {kBlr, kBlr}), pdata));
  INFO((result ? std::string() : result.error().message));
  CHECK(result);
}

REX_TEST_CASE("A pdata type 2 entry at a save run is a codegen error", "[codegen][millicode]") {
  rex::test::TempFolder folder;
  const rex::test::PDataEntry pdata[] = {
      {Address(0), 2}, {Address(kHelperIndex), 20, 0, rex::test::kPDataRestoreMillicode}};
  const auto result = rex::test::GenerateFromImage(
      folder, rex::test::BuildCodeImage(WithRun({Run::kGPRSave}, {kBlr, kBlr}), pdata), {}, false);
  REQUIRE_FALSE(result);
  CHECK(Has(result.error().message, "millicode"));
}

REX_TEST_CASE("A pdata type 2 entry that is not a helper does not take the handler path",
              "[codegen][millicode]") {
  rex::test::TempFolder folder;
  const rex::test::PDataEntry pdata[] = {{Address(0), 2, 0, rex::test::kPDataRestoreMillicode}};
  const auto result = rex::test::GenerateFromImage(
      folder, rex::test::BuildCodeImage(WithRun({Run::kGPRSave}, {kBlr, kBlr}), pdata), {}, false);
  REQUIRE_FALSE(result);
  CHECK(Has(result.error().message, "millicode"));
}
