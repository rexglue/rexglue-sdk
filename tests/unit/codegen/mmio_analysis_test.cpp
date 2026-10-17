/**
 * @file        tests/unit/codegen/mmio_analysis_test.cpp
 * @brief       Recompile time MMIO classification, the [mmio] tables, emitted macros and report
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "temp_folder.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/codegen/codegen_context.h>
#include <rex/codegen/codegen_writer.h>
#include <rex/codegen/config.h>
#include <rex/codegen/module_stub.h>
#include <rex/memory/byte_swap.h>
#include <rex/mmio.h>

#include "codegen/mmio_analysis.h"

namespace fs = std::filesystem;
using namespace rex::codegen;
using rex::u32;
using rex::u8;

namespace {

constexpr u32 kBase = 0x82000000;

constexpr u32 kLisGPU = 0x3D607FC8;
constexpr u32 kLisAPU = 0x3D607FEA;
constexpr u32 kLisWindow = 0x3D607F00;
constexpr u32 kLisDataHigh = 0x3D607FFF;
constexpr u32 kOriDataLow = 0x616BFFFF;
constexpr u32 kOriRegister = 0x616B6530;
constexpr u32 kMoveBase = 0x7D6A5B78;
constexpr u32 kLoadThroughCopy = 0x806A0010;
constexpr u32 kLoadWord = 0x806B0010;
constexpr u32 kLoadWordZero = 0x806B0000;
constexpr u32 kLoadIntoBase = 0x816B0000;
constexpr u32 kStoreWord = 0x906B0010;
constexpr u32 kLoadHalf = 0xA06B0010;
constexpr u32 kStoreThroughR4 = 0x90640000;
constexpr u32 kLoadThroughR4 = 0x80640000;
constexpr u32 kStoreDataOnStack = 0x91610000;
constexpr u32 kAddisOffset = 0x3D631FF2;
constexpr u32 kShiftOffset = 0x556B103A;
constexpr u32 kEieio = 0x7C0006AC;
constexpr u32 kBlr = 0x4E800020;
constexpr u32 kCompare = 0x2F030000;
constexpr u32 kBranchEqualPlus12 = 0x419A000C;
constexpr u32 kBranchPlus8 = 0x48000008;
constexpr u32 kBranchNotEqualMinus8 = 0x409AFFF8;
constexpr u32 kBranchToEntry = 0x4BFFFFEC;
constexpr u32 kFloatAdd = 0xFD61102A;
constexpr u32 kFloatLoad = 0xC02B0000;
constexpr u32 kFloatStore = 0xD02B0010;
constexpr u32 kCallFar = 0x48001001;
constexpr u32 kNop = 0x60000000;
constexpr u32 kMoveToCTR = 0x7D8903A6;
constexpr u32 kBranchCTR = 0x4E800420;
constexpr u32 kLisR3GPU = 0x3C607FC8;
constexpr u32 kLisR31GPU = 0x3FE07FC8;
constexpr u32 kLoadThroughR3 = 0x80830000;
constexpr u32 kLoadThroughR31 = 0x80BF0000;
constexpr u32 kLisR4Plain = 0x3C801000;

constexpr u32 Address(size_t index) {
  return kBase + static_cast<u32>(index * 4);
}

std::vector<u8> Encode(std::initializer_list<u32> words) {
  std::vector<u8> bytes(words.size() * 4);
  size_t offset = 0;
  for (u32 word : words) {
    rex::memory::store<be_u32>(bytes.data() + offset, word);
    offset += 4;
  }
  return bytes;
}

struct Program {
  std::vector<u8> bytes;
  std::vector<MMIOCodeBlock> blocks;
  std::vector<JumpTable> jump_tables;
  std::vector<u32> hook_sites;
  std::vector<u32> hook_targets;
  std::unordered_map<u32, MMIODeclaredAccess> declared;

  explicit Program(std::initializer_list<u32> words) : bytes(Encode(words)) {
    blocks.push_back({kBase, bytes});
  }

  MMIOClassification Classify() const {
    return ClassifyMMIO({blocks, jump_tables, hook_sites, hook_targets, &declared});
  }
};

const MMIOSite* SiteAt(const MMIOClassification& result, u32 address) {
  const auto it = std::ranges::find(result.sites, address, &MMIOSite::address);
  return it == result.sites.end() ? nullptr : &*it;
}

struct Scratch {
  rex::test::TempFolder folder;
  fs::path root;

  explicit Scratch(std::string_view name)
      : folder(std::string("rexglue_mmio_") + std::string(name) + "_"), root(folder.path) {}

  std::string Write(std::string_view body) const {
    auto path = root / "game.toml";
    std::ofstream(path, std::ios::binary)
        << "file_path = \"game.xex\"\nout_directory_path = \"out\"\n"
        << body;
    return path.string();
  }
};

}  // namespace

REX_TEST_CASE("A constant GPU base is proven", "[codegen][mmio]") {
  const auto result = Program{kLisGPU, kLoadWord, kBlr}.Classify();
  REQUIRE(result.sites.size() == 1);
  CHECK(result.At(Address(1)) == MMIOAccess::kGPU);
  CHECK(result.sites[0].reason == MMIOReason::kProven);
  CHECK(result.sites[0].target == 0x7FC80010u);
}

REX_TEST_CASE("A constant APU base built with ori and mr is proven", "[codegen][mmio]") {
  const auto result = Program{kLisAPU, kOriRegister, kMoveBase, kLoadThroughCopy, kBlr}.Classify();
  CHECK(result.At(Address(3)) == MMIOAccess::kAPU);
  REQUIRE(SiteAt(result, Address(3)) != nullptr);
  CHECK(SiteAt(result, Address(3))->target == 0x7FEA6540u);
}

REX_TEST_CASE("A proven 16 bit access and a proven store are classified", "[codegen][mmio]") {
  const auto result = Program{kLisGPU, kLoadHalf, kStoreWord, kBlr}.Classify();
  CHECK(result.At(Address(1)) == MMIOAccess::kGPU);
  CHECK(result.At(Address(2)) == MMIOAccess::kGPU);
}

REX_TEST_CASE("A float store is classified and a float load is unsupported", "[codegen][mmio]") {
  const auto result = Program{kLisGPU, kFloatStore, kFloatLoad, kBlr}.Classify();
  CHECK(result.At(Address(1)) == MMIOAccess::kGPU);
  const MMIOSite* load = SiteAt(result, Address(2));
  REQUIRE(load != nullptr);
  CHECK(load->access == MMIOAccess::kPlain);
  CHECK(load->reason == MMIOReason::kUnsupported);
  CHECK(load->target == 0x7FC80000u);
}

REX_TEST_CASE("A known address elsewhere in the 0x7F window is unserved", "[codegen][mmio]") {
  const auto result = Program{kLisWindow, kLoadWordZero, kBlr}.Classify();
  REQUIRE(result.sites.size() == 1);
  CHECK(result.sites[0].reason == MMIOReason::kUnserved);
  CHECK(result.sites[0].target == 0x7F000000u);
  CHECK(result.At(Address(1)) == MMIOAccess::kPlain);
}

REX_TEST_CASE("A 0x7F constant used as data stays plain", "[codegen][mmio]") {
  const auto result = Program{kLisDataHigh, kOriDataLow, kStoreDataOnStack, kBlr}.Classify();
  CHECK(result.sites.empty());
}

REX_TEST_CASE("The generic common load pattern with no barrier hints is guarded",
              "[codegen][mmio]") {
  const auto result = Program{kAddisOffset, kShiftOffset, kLoadWordZero, kBlr}.Classify();
  CHECK(result.At(Address(2)) == MMIOAccess::kGuarded);
  REQUIRE(SiteAt(result, Address(2)) != nullptr);
  CHECK(SiteAt(result, Address(2))->reason == MMIOReason::kOffset);
}

REX_TEST_CASE("An unknown base before eieio is guarded and a known plain base is not",
              "[codegen][mmio]") {
  const auto unknown = Program{kStoreThroughR4, kEieio, kBlr}.Classify();
  CHECK(unknown.At(Address(0)) == MMIOAccess::kGuarded);
  REQUIRE(SiteAt(unknown, Address(0)) != nullptr);
  CHECK(SiteAt(unknown, Address(0))->reason == MMIOReason::kBarrier);

  const auto known = Program{kLisR4Plain, kStoreThroughR4, kEieio, kBlr}.Classify();
  CHECK(known.sites.empty());
}

REX_TEST_CASE("A declared GPU read calls the GPU handler", "[codegen][mmio]") {
  Program program{kLoadThroughR4, kBlr};
  program.declared = {{Address(0), {rex::MMIOBlock::kGPU, false}}};
  const auto result = program.Classify();
  CHECK(result.At(Address(0)) == MMIOAccess::kGPU);
  const MMIOSite* site = SiteAt(result, Address(0));
  REQUIRE(site != nullptr);
  CHECK(site->reason == MMIOReason::kDeclared);
  CHECK_FALSE(site->write);
}

REX_TEST_CASE("A declared APU write calls the APU handler", "[codegen][mmio]") {
  Program program{kStoreThroughR4, kBlr};
  program.declared = {{Address(0), {rex::MMIOBlock::kAPU, true}}};
  const auto result = program.Classify();
  CHECK(result.At(Address(0)) == MMIOAccess::kAPU);
  const MMIOSite* site = SiteAt(result, Address(0));
  REQUIRE(site != nullptr);
  CHECK(site->reason == MMIOReason::kDeclared);
  CHECK(site->write);
}

REX_TEST_CASE("A base set before a loop is proven inside the loop", "[codegen][mmio]") {
  const auto result =
      Program{kLisGPU, kLoadWordZero, kCompare, kBranchNotEqualMinus8, kBlr}.Classify();
  CHECK(result.At(Address(1)) == MMIOAccess::kGPU);
}

REX_TEST_CASE("Predecessors with different bases leave the access plain", "[codegen][mmio]") {
  const auto result =
      Program{kCompare, kBranchEqualPlus12, kLisGPU, kBranchPlus8, kLisAPU, kLoadWordZero, kBlr}
          .Classify();
  CHECK(result.At(Address(5)) == MMIOAccess::kPlain);
  CHECK(result.sites.empty());
}

REX_TEST_CASE("Predecessors with the same base keep the proof", "[codegen][mmio]") {
  const auto result =
      Program{kCompare, kBranchEqualPlus12, kLisGPU, kBranchPlus8, kLisGPU, kLoadWordZero, kBlr}
          .Classify();
  CHECK(result.At(Address(5)) == MMIOAccess::kGPU);
}

REX_TEST_CASE("A call clears r0 and r3 to r12 and keeps r31", "[codegen][mmio]") {
  const auto result =
      Program{kLisR3GPU, kLisR31GPU, kCallFar, kLoadThroughR3, kLoadThroughR31, kBlr}.Classify();
  CHECK(result.At(Address(3)) == MMIOAccess::kPlain);
  CHECK(result.At(Address(4)) == MMIOAccess::kGPU);
}

REX_TEST_CASE("A load into the base register ends the proof", "[codegen][mmio]") {
  const auto result = Program{kLisGPU, kLoadIntoBase, kLoadWordZero, kBlr}.Classify();
  CHECK(result.At(Address(1)) == MMIOAccess::kGPU);
  CHECK(result.At(Address(2)) == MMIOAccess::kPlain);
}

REX_TEST_CASE("A float instruction between lis and the access keeps the proof", "[codegen][mmio]") {
  const auto result = Program{kLisGPU, kFloatAdd, kLoadWordZero, kBlr}.Classify();
  CHECK(result.At(Address(2)) == MMIOAccess::kGPU);
}

REX_TEST_CASE("A mid asm hook clears every register", "[codegen][mmio]") {
  Program program{kLisGPU, kNop, kLoadWordZero, kBlr};
  program.hook_sites = {Address(1)};
  CHECK(program.Classify().At(Address(2)) == MMIOAccess::kPlain);
}

REX_TEST_CASE("A jump table target inherits the state at its bctr", "[codegen][mmio]") {
  Program without_table{kLisGPU, kMoveToCTR, kBranchCTR, kLoadWordZero, kBlr};
  CHECK(without_table.Classify().At(Address(3)) == MMIOAccess::kPlain);

  Program with_table{kLisGPU, kMoveToCTR, kBranchCTR, kLoadWordZero, kBlr};
  with_table.jump_tables.push_back({Address(2), 0, 12, {Address(3)}});
  CHECK(with_table.Classify().At(Address(3)) == MMIOAccess::kGPU);
}

REX_TEST_CASE("Code after a gap between code blocks starts unknown", "[codegen][mmio]") {
  const std::vector<u8> first = Encode({kLisGPU});
  const std::vector<u8> second = Encode({kLoadWordZero, kBlr});
  const std::vector<MMIOCodeBlock> blocks{{kBase, first}, {kBase + 0x10, second}};
  const std::unordered_map<u32, MMIODeclaredAccess> declared;
  const auto result = ClassifyMMIO({blocks, {}, {}, {}, &declared});
  CHECK(result.At(kBase + 0x10) == MMIOAccess::kPlain);
}

REX_TEST_CASE("A branch back to the entry keeps the entry unknown", "[codegen][mmio]") {
  const std::vector<u8> entry = Encode({kLoadWordZero, kBlr});
  const std::vector<u8> tail = Encode({kLisGPU, kBranchToEntry});
  const std::vector<MMIOCodeBlock> blocks{{kBase, entry}, {kBase + 0x10, tail}};
  const std::unordered_map<u32, MMIODeclaredAccess> declared;
  const auto result = ClassifyMMIO({blocks, {}, {}, {}, &declared});
  CHECK(result.At(kBase) == MMIOAccess::kPlain);
}

REX_TEST_CASE("The [mmio.gpu] and [mmio.apu] tables load declared accesses", "[codegen][mmio]") {
  Scratch scratch("loads");
  RecompilerConfig config;
  REQUIRE(
      config.Load(scratch.Write("[mmio.gpu]\nread = [0x82000010]\nwrite = [0x82000014]\n"
                                "[mmio.apu]\nwrite = [0x82000020]\n")));
  const auto& declared = config.declared_mmio_accesses;
  REQUIRE(declared.size() == 3);
  CHECK(declared.at(0x82000010u).block == rex::MMIOBlock::kGPU);
  CHECK_FALSE(declared.at(0x82000010u).write);
  CHECK(declared.at(0x82000014u).block == rex::MMIOBlock::kGPU);
  CHECK(declared.at(0x82000014u).write);
  CHECK(declared.at(0x82000020u).block == rex::MMIOBlock::kAPU);
  CHECK(declared.at(0x82000020u).write);
}

REX_TEST_CASE("A misaligned declared address fails validation", "[codegen][mmio]") {
  RecompilerConfig config;
  config.file_path = "game.xex";
  config.out_directory_path = "out";
  config.declared_mmio_accesses = {{0x82000012, {rex::MMIOBlock::kGPU, false}}};
  const auto result = config.Validate();
  CHECK_FALSE(result.valid);
  CHECK(std::ranges::any_of(result.errors, [](const std::string& error) {
    return error.find("mmio address 0x82000012") != std::string::npos;
  }));
}

REX_TEST_CASE("An address declared twice fails the load", "[codegen][mmio]") {
  Scratch scratch("twice");
  RecompilerConfig config;
  CHECK_FALSE(config.Load(
      scratch.Write("[mmio.gpu]\nread = [0x82000010]\n[mmio.apu]\nwrite = [0x82000010]\n")));
}

namespace {

struct EmitFixture {
  rex::test::TempFolder folder;
  fs::path root;
  std::vector<u8> data;
  StubModule module;
  CodegenContext ctx;

  EmitFixture(std::string_view name, std::initializer_list<u32> words,
              std::initializer_list<u32> function_sizes,
              std::unordered_map<u32, MMIODeclaredAccess> declared = {})
      : folder(std::string("rexglue_mmio_emit_") + std::string(name) + "_"),
        root(folder.path),
        data(Encode(words)),
        ctx(MakeContext(root, module, data, std::move(declared))) {
    u32 address = kBase;
    for (u32 size : function_sizes) {
      auto* node = ctx.graph.AddFunction(address, size, FunctionAuthority::kDiscovered, true);
      REQUIRE(node != nullptr);
      node->Discover({{address, size}}, {}, {});
      node->Seal();
      address += size;
    }
  }

  static CodegenContext MakeContext(const fs::path& root, StubModule& module,
                                    const std::vector<u8>& data,
                                    std::unordered_map<u32, MMIODeclaredAccess> declared) {
    module.Load(kBase, data.data(), data.size());

    RecompilerConfig config;
    config.project_name = "testproj";
    config.out_directory_path = "generated";
    config.declared_mmio_accesses = std::move(declared);

    CodegenContext ctx = CodegenContext::Create(BinaryView::FromModule(module), std::move(config));
    ctx.SetConfigDir(root);
    ctx.GetAnalysisState().format = "xex";
    ctx.GetAnalysisState().load_address = kBase;
    ctx.GetAnalysisState().entry_point = kBase;
    ctx.GetAnalysisState().image_size = static_cast<u32>(data.size());
    return ctx;
  }

  std::string Read(std::string_view file) const {
    std::ifstream in(root / "generated" / fs::path(file), std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }
};

bool Has(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

REX_TEST_CASE("Codegen emits the macro each classified access needs", "[codegen][mmio]") {
  EmitFixture fixture("macros",
                      {kLisGPU, kLoadWord, kBlr, kStoreThroughR4, kEieio, kBlr, kLisGPU, kLoadHalf,
                       kBlr, kLisAPU, kStoreWord, kBlr},
                      {12, 12, 12, 12});
  CodegenWriter writer(fixture.ctx);
  REQUIRE(writer.Write(false));
  const std::string code = fixture.Read("testproj_recomp.0.cpp");
  CHECK(Has(code, "REX_GPU_LOAD32(ctx.r11.u32 + 16)"));
  CHECK(Has(code, "REX_MM_STORE_U32("));
  CHECK(Has(code, "REX_MM_LOAD_U16("));
  CHECK(Has(code, "REX_APU_STORE32("));
  CHECK_FALSE(Has(code, "REX_MM_LOAD_U32("));
}

REX_TEST_CASE("Codegen writes every classified site to codegen.mmio.json", "[codegen][mmio]") {
  EmitFixture fixture("report", {kLisGPU, kLoadWord, kBlr, kStoreThroughR4, kEieio, kBlr},
                      {12, 12});
  CodegenWriter writer(fixture.ctx);
  REQUIRE(writer.Write(false));
  const std::string report = fixture.Read("codegen.mmio.json");
  CHECK(Has(report, "\"proven\""));
  CHECK(Has(report, "\"guarded\""));
  CHECK(Has(report, "\"unserved\""));
  CHECK(Has(report, "\"unsupported\""));
  CHECK(Has(report, "\"0x82000004\""));
  CHECK(Has(report, "\"0x7FC80010\""));
  CHECK(Has(report, "\"0x8200000C\""));
  CHECK(Has(report, "\"eieio\""));
  CHECK(Has(report, "\"declared\""));
  CHECK(Has(report, "\"access\": \"read\""));
  CHECK(Has(report, "\"access\": \"write\""));
}

REX_TEST_CASE("A declared GPU read emits the direct call", "[codegen][mmio]") {
  EmitFixture fixture("declared", {kLoadThroughR4, kBlr}, {8},
                      {{kBase, {rex::MMIOBlock::kGPU, false}}});
  CodegenWriter writer(fixture.ctx);
  REQUIRE(writer.Write(false));
  CHECK(Has(fixture.Read("testproj_recomp.0.cpp"), "REX_GPU_LOAD32("));
}

REX_TEST_CASE("A declaration that matches no load or store fails the write", "[codegen][mmio]") {
  EmitFixture fixture("unmatched", {kBlr}, {4}, {{kBase, {rex::MMIOBlock::kGPU, false}}});
  CodegenWriter writer(fixture.ctx);
  CHECK_FALSE(writer.Write(false));
}

REX_TEST_CASE("A read declaration on a store fails the write", "[codegen][mmio]") {
  EmitFixture fixture("direction", {kStoreThroughR4, kBlr}, {8},
                      {{kBase, {rex::MMIOBlock::kGPU, false}}});
  CodegenWriter writer(fixture.ctx);
  CHECK_FALSE(writer.Write(false));
}

REX_TEST_CASE("A declaration that disagrees with the proven block fails the write",
              "[codegen][mmio]") {
  EmitFixture fixture("disagree", {kLisGPU, kLoadWord, kBlr}, {12},
                      {{kBase + 4, {rex::MMIOBlock::kAPU, false}}});
  CodegenWriter writer(fixture.ctx);
  CHECK_FALSE(writer.Write(false));
}

REX_TEST_CASE("A declared address that is not an integer fails the load", "[codegen][mmio]") {
  Scratch scratch("string");
  RecompilerConfig config;
  CHECK_FALSE(config.Load(scratch.Write("[mmio.gpu]\nread = [\"0x82000010\"]\n")));
}
