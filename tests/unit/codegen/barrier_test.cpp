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
#include "temp_folder.h"
#include "ppc_words.h"

#include <filesystem>
#include <fstream>
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
constexpr u32 kSync = 0x7C0004AC;
constexpr u32 kLwsync = 0x7C2004AC;
constexpr u32 kEieio = 0x7C0006AC;
constexpr u32 kBlr = 0x4E800020;

std::string EmitFunction(std::string_view name, u32 barrier) {
  const rex::test::TempFolder folder(std::string("rexglue_barrier_") + std::string(name) + "_");
  const fs::path& root = folder.path;
  const std::vector<u8> data = Encode({barrier, kBlr});
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

bool Contains(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

REX_TEST_CASE("sync emits a sequentially consistent fence", "[codegen][barrier]") {
  CHECK(Contains(EmitFunction("sync", kSync),
                 "std::atomic_thread_fence(std::memory_order_seq_cst);"));
}

REX_TEST_CASE("lwsync emits an acquire release fence", "[codegen][barrier]") {
  CHECK(Contains(EmitFunction("lwsync", kLwsync),
                 "std::atomic_thread_fence(std::memory_order_acq_rel);"));
}

REX_TEST_CASE("eieio emits a sequentially consistent fence", "[codegen][barrier]") {
  CHECK(Contains(EmitFunction("eieio", kEieio),
                 "std::atomic_thread_fence(std::memory_order_seq_cst);"));
}
