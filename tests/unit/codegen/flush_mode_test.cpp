/**
 * @file        tests/unit/codegen/flush_mode_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "temp_folder.h"
#include "ppc_words.h"

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <rex/codegen/codegen_context.h>
#include <rex/codegen/codegen_writer.h>
#include <rex/codegen/module_stub.h>

namespace fs = std::filesystem;
using namespace rex::codegen;

namespace {

using rex::test::Encode;

constexpr u32 kBase = 0x82000000;
constexpr u32 kVaddfp = 0x1020180A;
constexpr u32 kVsubfp = 0x1040184A;
constexpr u32 kMtvscr = 0x10001644;
constexpr u32 kFadd = 0xFC22182A;
constexpr u32 kBlr = 0x4E800020;
constexpr std::string_view kSet = "ctx.fpscr.SetFlushMode(ctx.vscr_nj);";

std::string EmitFunction(std::string_view name, std::initializer_list<u32> words) {
  const rex::test::TempFolder folder(std::string("rexglue_flush_mode_") + std::string(name) + "_");
  const fs::path& root = folder.path;
  const std::vector<u8> data = Encode(words);
  StubModule module;
  module.Load(kBase, data.data(), data.size());

  RecompilerConfig config;
  config.project_name = "testproj";
  config.out_directory_path = "generated";
  CodegenContext ctx = CodegenContext::Create(BinaryView::FromModule(module), std::move(config));
  ctx.SetConfigDir(root);
  ctx.GetAnalysisState().format = "xex";
  ctx.GetAnalysisState().load_address = kBase;
  ctx.GetAnalysisState().entry_point = kBase;
  ctx.GetAnalysisState().image_size = static_cast<u32>(data.size());
  const u32 size = static_cast<u32>(data.size());
  auto* node = ctx.graph.AddFunction(kBase, size, FunctionAuthority::kDiscovered, true);
  REQUIRE(node != nullptr);
  node->Discover({{kBase, size}}, {}, {});
  node->Seal();

  CodegenWriter writer(ctx);
  REQUIRE(writer.Write(false));
  std::ifstream in(root / "generated" / "testproj_recomp.0.cpp", std::ios::binary);
  std::string code{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  return code;
}

size_t Count(const std::string& text, std::string_view needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
    ++count;
  }
  return count;
}

}  // namespace

REX_TEST_CASE("a run of vector float operations sets the host flush mode once from VSCR NJ",
              "[codegen][flush]") {
  const std::string code = EmitFunction("run", {kVaddfp, kVsubfp, kBlr});
  CHECK(Count(code, kSet) == 1);
  CHECK(Count(code, "EnableFlushMode") == 0);
}

REX_TEST_CASE("a vector float operation after a scalar one sets the host flush mode from VSCR NJ",
              "[codegen][flush]") {
  const std::string code = EmitFunction("scalar_then_vector", {kFadd, kVaddfp, kBlr});
  const size_t disable = code.find("DisableFlushMode");
  const size_t set = code.find(kSet);
  CHECK(disable != std::string::npos);
  CHECK(set != std::string::npos);
  CHECK(disable < set);
  CHECK(Count(code, kSet) == 1);
}

REX_TEST_CASE("a scalar operation after a vector float one turns the host flush mode off",
              "[codegen][flush]") {
  const std::string code = EmitFunction("vector_then_scalar", {kVaddfp, kFadd, kBlr});
  const size_t set = code.find(kSet);
  const size_t disable = code.find("DisableFlushModeUnconditional");
  CHECK(set != std::string::npos);
  CHECK(disable != std::string::npos);
  CHECK(set < disable);
}

REX_TEST_CASE("mtvscr between vector float operations applies the new NJ bit to the host",
              "[codegen][flush]") {
  const std::string code = EmitFunction("mtvscr_vector", {kVaddfp, kMtvscr, kVsubfp, kBlr});
  CHECK(Count(code, kSet) == 2);
  const size_t store = code.find("ctx.vscr_nj = ");
  const size_t second = code.find(kSet, code.find(kSet) + 1);
  CHECK(store != std::string::npos);
  CHECK(store < second);
}

REX_TEST_CASE("mtvscr between scalar operations leaves the host flush mode alone",
              "[codegen][flush]") {
  const std::string code = EmitFunction("mtvscr_scalar", {kFadd, kMtvscr, kFadd, kBlr});
  CHECK(Count(code, kSet) == 0);
  CHECK(Count(code, "EnableFlushMode") == 0);
}
