/**
 * @file        tests/unit/codegen/barrier_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "support/temp_folder.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <rex/codegen/codegen_context.h>
#include <rex/codegen/codegen_writer.h>
#include <rex/codegen/test_support.h>

namespace fs = std::filesystem;
using namespace rex::codegen;

namespace {

constexpr u32 kBase = 0x82000000;
constexpr u32 kSync = 0x7C0004AC;
constexpr u32 kLwsync = 0x7C2004AC;
constexpr u32 kEieio = 0x7C0006AC;
constexpr u32 kBlr = 0x4E800020;

std::vector<u8> Encode(std::initializer_list<u32> words) {
  std::vector<u8> data;
  for (const u32 word : words) {
    data.insert(data.end(), {static_cast<u8>(word >> 24), static_cast<u8>(word >> 16),
                             static_cast<u8>(word >> 8), static_cast<u8>(word)});
  }
  return data;
}

std::string EmitFunction(std::string_view name, u32 barrier) {
  const fs::path root = fs::temp_directory_path() / "rexglue_barrier_test" / fs::path(name);
  fs::remove_all(root);
  fs::create_directories(root);
  const std::vector<u8> data = Encode({barrier, kBlr});
  TestModule module;
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
  auto* node = ctx.graph.AddFunction(kBase, 8, FunctionAuthority::kDiscovered, true);
  REQUIRE(node != nullptr);
  node->Discover({{kBase, 8}}, {}, {});
  node->Seal();

  CodegenWriter writer(ctx);
  REQUIRE(writer.Write(false));
  std::ifstream in(root / "generated" / "testproj_recomp.0.cpp", std::ios::binary);
  std::string code{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  rex::test::RemoveTree(root);
  return code;
}

bool Has(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

REX_TEST_CASE("sync emits a sequentially consistent fence", "[codegen][barrier]") {
  CHECK(Has(EmitFunction("sync", kSync), "std::atomic_thread_fence(std::memory_order_seq_cst);"));
}

REX_TEST_CASE("lwsync emits an acquire release fence", "[codegen][barrier]") {
  CHECK(
      Has(EmitFunction("lwsync", kLwsync), "std::atomic_thread_fence(std::memory_order_acq_rel);"));
}

REX_TEST_CASE("eieio emits a sequentially consistent fence", "[codegen][barrier]") {
  CHECK(Has(EmitFunction("eieio", kEieio),
            "std::atomic_thread_fence(std::memory_order_seq_cst);"));
}
