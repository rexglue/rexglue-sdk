/**
 * @file        tests/unit/codegen/codegen_writer_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "temp_folder.h"
#include "file_text.h"

#include <format>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "codegen/codegen_context.h"
#include "codegen/codegen_writer.h"
#include "codegen/module_stub.h"

namespace fs = std::filesystem;
using namespace rex::codegen;

namespace {

using rex::test::ReadText;

constexpr u32 kBaseAddress = 0x82000000;
constexpr u32 kFunctionCount = 4;

// One `blr` (0x4E800020) per function.
std::vector<u8> BuildBlrBinary(u32 function_count) {
  std::vector<u8> data;
  data.reserve(size_t{function_count} * 4);
  for (u32 i = 0; i < function_count; ++i) {
    data.insert(data.end(), {0x4E, 0x80, 0x00, 0x20});
  }
  return data;
}

struct WriterFixture {
  rex::test::TempFolder folder;
  fs::path root;
  std::vector<u8> data;
  StubModule module;
  std::unique_ptr<CodegenContext> ctx;

  explicit WriterFixture(std::string_view name, u32 function_count = kFunctionCount)
      : folder(std::string("rexglue_writer_") + std::string(name) + "_"),
        root(folder.path),
        data(BuildBlrBinary(function_count)) {
    module.Load(kBaseAddress, data.data(), data.size());

    RecompilerConfig config;
    config.project_name = "testproj";
    config.out_directory_path = "generated";

    ctx = std::make_unique<CodegenContext>(
        CodegenContext::Create(BinaryView::FromModule(module), std::move(config)));
    ctx->SetConfigDir(root);
    ctx->GetAnalysisState().format = "xex";
    ctx->GetAnalysisState().load_address = kBaseAddress;
    ctx->GetAnalysisState().entry_point = kBaseAddress;
    ctx->GetAnalysisState().image_size = static_cast<u32>(data.size());

    for (u32 i = 0; i < function_count; ++i) {
      u32 addr = kBaseAddress + i * 4;
      auto* node = ctx->graph.AddFunction(addr, 4, FunctionAuthority::kDiscovered, true);
      REQUIRE(node != nullptr);
      node->Discover({{addr, 4}}, {}, {});
      node->Seal();
    }
  }

  fs::path GetOutputDir() const { return root / "generated"; }
};

}  // namespace

REX_TEST_CASE("Second write with unchanged inputs writes nothing", "[codegen][codegen_writer]") {
  WriterFixture fx("unchanged");

  CodegenWriter first(*fx.ctx);
  REQUIRE(first.Write(false));
  REQUIRE_FALSE(first.GetWrittenFiles().empty());

  CodegenWriter second(*fx.ctx);
  REQUIRE(second.Write(false));

  CHECK(second.GetWrittenFiles().empty());
  CHECK(second.GetUnchangedFiles().size() == first.GetWrittenFiles().size());
  CHECK(second.GetDeletedFiles().empty());
}

REX_TEST_CASE("Unchanged files keep their modification time", "[codegen][codegen_writer]") {
  WriterFixture fx("mtime");

  CodegenWriter first(*fx.ctx);
  REQUIRE(first.Write(false));

  auto init_header = fx.GetOutputDir() / "testproj_init.h";
  REQUIRE(fs::exists(init_header));
  auto before = fs::last_write_time(init_header);

  CodegenWriter second(*fx.ctx);
  REQUIRE(second.Write(false));

  const bool unchanged = fs::last_write_time(init_header) == before;
  CHECK(unchanged);
}

REX_TEST_CASE("Stale generated files are swept after the write", "[codegen][codegen_writer]") {
  WriterFixture fx("stale");

  CodegenWriter first(*fx.ctx);
  REQUIRE(first.Write(false));

  auto orphan = fx.GetOutputDir() / "testproj_recomp.99.cpp";
  { std::ofstream(orphan) << "// leftover\n"; }
  REQUIRE(fs::exists(orphan));

  CodegenWriter second(*fx.ctx);
  REQUIRE(second.Write(false));

  CHECK_FALSE(fs::exists(orphan));
  CHECK(std::find(second.GetDeletedFiles().begin(), second.GetDeletedFiles().end(),
                  std::string("testproj_recomp.99.cpp")) != second.GetDeletedFiles().end());
}

REX_TEST_CASE("Files left by an earlier project prefix are swept", "[codegen][codegen_writer]") {
  WriterFixture fx("old_prefix");

  CodegenWriter first(*fx.ctx);
  REQUIRE(first.Write(false));

  const std::vector<std::string> old_names = {
      "oldname_pch.h",        "oldname_funcs.h",   "oldname_init.h",       "oldname_init.cpp",
      "oldname_register.cpp", "oldname_funcs.0.h", "oldname_recomp.12.cpp"};
  for (const auto& name : old_names) {
    std::ofstream(fx.GetOutputDir() / name) << "// leftover\n";
  }
  std::ofstream(fx.GetOutputDir() / "oldname_notes.txt") << "keep me\n";
  std::ofstream(fx.GetOutputDir() / "oldname_helper.cpp") << "// hand written\n";

  CodegenWriter second(*fx.ctx);
  REQUIRE(second.Write(false));

  for (const auto& name : old_names) {
    CHECK_FALSE(fs::exists(fx.GetOutputDir() / name));
  }
  CHECK(fs::exists(fx.GetOutputDir() / "oldname_notes.txt"));
  CHECK(fs::exists(fx.GetOutputDir() / "oldname_helper.cpp"));
}

REX_TEST_CASE("sources.cmake names the prefix of the generated files",
              "[codegen][codegen_writer]") {
  WriterFixture fx("prefix_var");

  CodegenWriter writer(*fx.ctx);
  REQUIRE(writer.Write(false));

  const std::string text = ReadText(fx.GetOutputDir() / "sources.cmake");
  CHECK(text.find("set(GENERATED_PREFIX " + std::string(1, '"') + "testproj" + std::string(1, '"') +
                  ")") != std::string::npos);
}

REX_TEST_CASE("Unrelated files in the output directory survive the sweep",
              "[codegen][codegen_writer]") {
  WriterFixture fx("bystander");

  CodegenWriter first(*fx.ctx);
  REQUIRE(first.Write(false));

  auto bystander = fx.GetOutputDir() / "notes.txt";
  { std::ofstream(bystander) << "keep me\n"; }

  CodegenWriter second(*fx.ctx);
  REQUIRE(second.Write(false));

  CHECK(fs::exists(bystander));
}

REX_TEST_CASE("The stamp is owned by the recompiler and survives the sweep",
              "[codegen][codegen_writer]") {
  WriterFixture fx("stamp");

  CodegenWriter first(*fx.ctx);
  REQUIRE(first.Write(false));

  auto stamp = fx.GetOutputDir() / "codegen.stamp";
  { std::ofstream(stamp) << "{}\n"; }

  CodegenWriter second(*fx.ctx);
  REQUIRE(second.Write(false));

  CHECK(fs::exists(stamp));
  CHECK(second.GetDeletedFiles().empty());
}

REX_TEST_CASE("A changed function rewrites only the files that differ",
              "[codegen][codegen_writer]") {
  WriterFixture fx("changed");

  CodegenWriter first(*fx.ctx);
  REQUIRE(first.Write(false));
  auto initial_count = first.GetWrittenFiles().size();
  REQUIRE(initial_count > 1);

  // Touches init.h, init.cpp, register.cpp and one recomp file, not sources.cmake.
  fx.ctx->graph.SetFunctionName(kBaseAddress + 4, "renamed_function");

  CodegenWriter second(*fx.ctx);
  REQUIRE(second.Write(false));

  CHECK_FALSE(second.GetWrittenFiles().empty());
  CHECK(second.GetWrittenFiles().size() < initial_count);
  CHECK(std::find(second.GetWrittenFiles().begin(), second.GetWrittenFiles().end(),
                  std::string("sources.cmake")) == second.GetWrittenFiles().end());
}

REX_TEST_CASE("Recomp files are stable when one function name gets longer",
              "[codegen][codegen_writer]") {
  WriterFixture fx("partition", 8);

  CodegenWriter first(*fx.ctx);
  REQUIRE(first.Write(false));

  fx.ctx->graph.SetFunctionName(kBaseAddress, "a_considerably_longer_function_name");

  CodegenWriter second(*fx.ctx);
  REQUIRE(second.Write(false));

  size_t rewritten_recomp_files = 0;
  for (const auto& name : second.GetWrittenFiles()) {
    if (name.find("_recomp.") != std::string::npos)
      ++rewritten_recomp_files;
  }
  CHECK(rewritten_recomp_files <= 1);
  CHECK(fs::exists(fx.GetOutputDir() / "codegen.partition.json"));
}

REX_TEST_CASE("Every name a recomp file calls is declared in its header",
              "[codegen][codegen_writer]") {
  WriterFixture fx("declarations", 8);

  CodegenWriter writer(*fx.ctx);
  REQUIRE(writer.Write(false));

  static const std::regex kCallRe(R"(\b([A-Za-z_][A-Za-z0-9_]*)\(ctx, base\))");
  static const std::regex kDeclRe(R"(DECLARE_REX_FUNC\(([A-Za-z_][A-Za-z0-9_]*)\))");

  for (const auto& entry : fs::directory_iterator(fx.GetOutputDir())) {
    auto name = entry.path().filename().string();
    if (name.find("_recomp.") == std::string::npos)
      continue;

    auto index = name.substr(name.find("_recomp.") + 8);
    index = index.substr(0, index.find('.'));
    auto header = fx.GetOutputDir() / std::format("testproj_funcs.{}.h", index);
    REQUIRE(fs::exists(header));

    std::string body = ReadText(entry.path());
    std::string decls = ReadText(header);

    std::set<std::string> declared;
    for (std::sregex_iterator it(decls.begin(), decls.end(), kDeclRe), end; it != end; ++it) {
      declared.insert((*it)[1].str());
    }

    // A call may precede its definition, so only a declaration makes it valid.
    for (std::sregex_iterator it(body.begin(), body.end(), kCallRe), end; it != end; ++it) {
      auto called = (*it)[1].str();
      INFO("file " << name << " calls " << called << " with no declaration");
      CHECK(declared.contains(called));
    }
  }
}
