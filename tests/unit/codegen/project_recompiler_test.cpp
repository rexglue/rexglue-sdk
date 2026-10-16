/**
 * @file        tests/unit/codegen/project_recompiler_test.cpp
 * @brief       Code generation for a project whose DLL modules share an image base
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "support/temp_folder.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <rex/codegen/manifest.h>
#include <rex/codegen/project_recompiler.h>
#include <rex/filesystem/xex.h>
#include <rex/logging/config.h>
#include <rex/logging/logger.h>
#include <rex/logging/log.h>

#include "core/xex_builder.h"
#include "pe/pe_image.h"
#include "rexglue/log_categories.h"
#include "test_memory.h"

namespace fs = std::filesystem;

namespace {

constexpr u32 kEntryBase = 0x82000000;
constexpr u32 kDLLBase = 0x8E000000;
constexpr u32 kSecondDLLBase = 0x8F000000;
constexpr u32 kImageSize = 0x10000;
constexpr u32 kCodeOffset = 0x1000;
constexpr u32 kPDataOffset = 0x2000;
constexpr u32 kFunctionWords = 2;
constexpr u32 kLoadFirst = 0x38600111;
constexpr u32 kLoadSecond = 0x38600222;
constexpr u32 kReturn = 0x4E800020;

std::vector<u8> BuildImage(u32 base, u32 immediate_instruction) {
  std::vector<u8> image(kImageSize, 0);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
  dos->e_magic = IMAGE_DOS_SIGNATURE;
  dos->e_cblp = 0x90;
  dos->e_lfanew = 0x80;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(image.data() + 0x80);
  nt->Signature = IMAGE_NT_SIGNATURE;
  nt->FileHeader.Machine = IMAGE_FILE_MACHINE_POWERPCBE;
  nt->FileHeader.NumberOfSections = 2;
  nt->FileHeader.SizeOfOptionalHeader = IMAGE_SIZEOF_NT_OPTIONAL_HEADER;
  nt->FileHeader.Characteristics = IMAGE_FILE_32BIT_MACHINE;
  nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
  nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_XBOX;
  nt->OptionalHeader.SizeOfHeaders = kCodeOffset;
  nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress = kPDataOffset;
  nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size = 8;
  auto* section = reinterpret_cast<IMAGE_SECTION_HEADER*>(image.data() + 0x80 + 24 +
                                                          IMAGE_SIZEOF_NT_OPTIONAL_HEADER);
  std::memcpy(section->Name, ".text", 5);
  section->VirtualAddress = kCodeOffset;
  section->Misc.VirtualSize = 0x200;
  section->Characteristics = rex::filesystem::kPESectionMemoryExecute;
  auto* pdata = section + 1;
  std::memcpy(pdata->Name, ".pdata", 6);
  pdata->VirtualAddress = kPDataOffset;
  pdata->Misc.VirtualSize = 0x100;
  rex::test::PutBE32(image, kPDataOffset, base + kCodeOffset);
  rex::test::PutBE32(image, kPDataOffset + 4, (kFunctionWords << 8) | (1u << 30));
  rex::test::PutBE32(image, kCodeOffset, immediate_instruction);
  rex::test::PutBE32(image, kCodeOffset + 4, kReturn);
  rex::test::PutBE32(image, kCodeOffset + 8, immediate_instruction);
  rex::test::PutBE32(image, kCodeOffset + 12, kReturn);
  return image;
}

void WriteImage(const fs::path& path, u32 base, u32 immediate_instruction) {
  rex::test::XEXSpec spec;
  spec.load_address = base;
  spec.entry_point = base + kCodeOffset;
  spec.page_size_4kb = false;
  spec.image = BuildImage(base, immediate_instruction);
  const auto file = rex::test::BuildXEX(spec);
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
}

std::string ReadTree(const fs::path& root) {
  std::string text;
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::ifstream in(entry.path(), std::ios::binary);
    text.append(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  return text;
}

rex::Result<void> RunProject(const rex::test::TempFolder& folder, u32 entry_base, u32 first_base,
                             u32 second_base, std::string_view shared_text = {},
                             std::string_view extra_text = {},
                             std::vector<std::string> targets = {}) {
  WriteImage(folder.path / "entry.xex", entry_base, kLoadFirst);
  WriteImage(folder.path / "a.xex", first_base, kLoadFirst);
  WriteImage(folder.path / "b.xex", second_base, kLoadSecond);
  const auto manifest_path = folder.path / "demo_manifest.toml";
  std::ofstream(manifest_path) << shared_text << R"(
[project]
name = "demo"
[entrypoint]
file_path = "entry.xex"
out_directory_path = "generated/entry"
[[modules]]
file_path = "a.xex"
guest_path = "a.xex"
out_directory_path = "generated/a"
[[modules]]
file_path = "b.xex"
guest_path = "b.xex"
out_directory_path = "generated/b"
)" << extra_text;
  const auto manifest = rex::codegen::ManifestConfig::Load(manifest_path);
  REQUIRE(manifest);

  rex::testing::ReleaseTestMemory();
  rex::LogConfig config;
  config.debugger = false;
  config.categories.assign(rexglue::kLogCategories.begin(), rexglue::kLogCategories.end());
  REQUIRE(rex::log::Start(config).has_value());
  rex::codegen::ProjectRecompiler recompiler(*manifest);
  rex::codegen::ProjectRecompilerOptions options;
  options.force = true;
  options.targets = std::move(targets);
  auto result = recompiler.Run(options);
  rex::log::Stop();
  return result;
}

}  // namespace

REX_TEST_CASE("Code generation reads each DLL module from its own bytes at a shared base",
              "[codegen][project]") {
  rex::test::TempFolder folder;
  const auto result = RunProject(folder, kEntryBase, kDLLBase, kDLLBase);
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);

  const std::string first = ReadTree(folder.path / "generated" / "a");
  const std::string second = ReadTree(folder.path / "generated" / "b");
  CHECK(first.find("273") != std::string::npos);
  CHECK(first.find("546") == std::string::npos);
  CHECK(second.find("546") != std::string::npos);
  CHECK(second.find("273") == std::string::npos);
}

REX_TEST_CASE("Code generation refuses a DLL module whose image overlaps the entrypoint",
              "[codegen][project]") {
  rex::test::TempFolder folder;
  const auto result = RunProject(folder, kEntryBase, kEntryBase, kDLLBase);
  REQUIRE_FALSE(result);
  CHECK(result.error().message.find("Failed to load DLL module") != std::string::npos);
}

REX_TEST_CASE("Code generation defines a stub for an import from another user module",
              "[codegen][project]") {
  constexpr u32 kUserThunk = kEntryBase + 0x1100;
  constexpr u32 kKernelThunk = kEntryBase + 0x1110;
  rex::test::TempFolder folder;
  rex::test::XEXSpec spec;
  spec.load_address = kEntryBase;
  spec.entry_point = kEntryBase + kCodeOffset;
  spec.page_size_4kb = false;
  spec.image = BuildImage(kEntryBase, kLoadFirst);
  rex::test::PutBE32(spec.image, 0x1100, (1u << 24) | 7);
  rex::test::PutBE32(spec.image, 0x1110, (1u << 24) | 0x7FFE);
  spec.imports.push_back({"liba.dll", {kUserThunk}, 0});
  spec.imports.push_back({"xboxkrnl.exe", {kKernelThunk}, 0});
  const auto file = rex::test::BuildXEX(spec);
  std::ofstream(folder.path / "entry.xex", std::ios::binary)
      .write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
  const auto manifest_path = folder.path / "demo_manifest.toml";
  std::ofstream(manifest_path) << R"([project]
name = "demo"
[entrypoint]
file_path = "entry.xex"
out_directory_path = "generated/entry"
)";
  const auto manifest = rex::codegen::ManifestConfig::Load(manifest_path);
  REQUIRE(manifest);

  rex::testing::ReleaseTestMemory();
  rex::LogConfig config;
  config.debugger = false;
  config.categories.assign(rexglue::kLogCategories.begin(), rexglue::kLogCategories.end());
  REQUIRE(rex::log::Start(config).has_value());
  rex::codegen::ProjectRecompiler recompiler(*manifest);
  rex::codegen::ProjectRecompilerOptions options;
  options.force = true;
  const auto result = recompiler.Run(options);
  rex::log::Stop();
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);

  const std::string registration = ReadTree(folder.path / "generated" / "entry");
  CHECK(registration.find("DEFINE_REX_FUNC(sub_82001100)") != std::string::npos);
  CHECK(registration.find("ModuleImport target{\"liba\", 7}") != std::string::npos);
  CHECK(registration.find("SetFunction(0x82001100, sub_82001100)") != std::string::npos);
  CHECK(registration.find("DEFINE_REX_FUNC(sub_82001110)") == std::string::npos);
  CHECK(registration.find("ModuleImport target{\"xboxkrnl\"") == std::string::npos);
}

REX_TEST_CASE("A top level midasm hook reaches the entrypoint", "[codegen][project]") {
  rex::test::TempFolder folder;
  const auto result = RunProject(folder, kEntryBase, kDLLBase, kSecondDLLBase,
                                 R"([[midasm_hook]]
address = 0x82001000
name = "shared_hook"
)");
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  CHECK(ReadTree(folder.path / "generated" / "entry").find("shared_hook") != std::string::npos);
}

REX_TEST_CASE("A shared address goes to the binary whose image contains it", "[codegen][project]") {
  rex::test::TempFolder folder;
  const auto result = RunProject(folder, kEntryBase, kDLLBase, kSecondDLLBase,
                                 R"([[midasm_hook]]
address = 0x8F001000
name = "second_hook"
)");
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  CHECK(ReadTree(folder.path / "generated" / "b").find("second_hook") != std::string::npos);
  CHECK(ReadTree(folder.path / "generated" / "a").find("second_hook") == std::string::npos);
  CHECK(ReadTree(folder.path / "generated" / "entry").find("second_hook") == std::string::npos);
}

REX_TEST_CASE("A binary's own value overrides the shared one", "[codegen][project]") {
  rex::test::TempFolder folder;
  const auto result = RunProject(folder, kEntryBase, kDLLBase, kSecondDLLBase,
                                 R"([[midasm_hook]]
address = 0x82001000
name = "shared_hook"
)",
                                 R"([[entrypoint.midasm_hook]]
address = 0x82001000
name = "own_hook"
)");
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  const std::string tree = ReadTree(folder.path / "generated" / "entry");
  CHECK(tree.find("own_hook") != std::string::npos);
  CHECK(tree.find("shared_hook") == std::string::npos);
}

REX_TEST_CASE("A shared address inside no image fails and names the address",
              "[codegen][project]") {
  rex::test::TempFolder folder;
  const auto result = RunProject(folder, kEntryBase, kDLLBase, kSecondDLLBase,
                                 R"([[midasm_hook]]
address = 0x90001000
name = "lost_hook"
)");
  REQUIRE_FALSE(result);
  CHECK(result.error().message.find("0x90001000") != std::string::npos);
  CHECK(result.error().message.find("[[midasm_hook]]") != std::string::npos);
}

REX_TEST_CASE("A top level includes file loads into the shared layer", "[codegen][project]") {
  rex::test::TempFolder folder;
  std::ofstream(folder.path / "shared.toml") << R"([[midasm_hook]]
address = 0x82001000
name = "included_hook"
)";
  const auto result = RunProject(folder, kEntryBase, kDLLBase, kSecondDLLBase,
                                 R"(includes = ["shared.toml"]
)");
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  CHECK(ReadTree(folder.path / "generated" / "entry").find("included_hook") != std::string::npos);
}

REX_TEST_CASE("A per binary midasm hook works without a shared layer", "[codegen][project]") {
  rex::test::TempFolder folder;
  const auto result = RunProject(folder, kEntryBase, kDLLBase, kSecondDLLBase, {},
                                 R"([[entrypoint.midasm_hook]]
address = 0x82001000
name = "own_hook"
)");
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  CHECK(ReadTree(folder.path / "generated" / "entry").find("own_hook") != std::string::npos);
}

REX_TEST_CASE("A top level functions table reaches only the binary that contains the address",
              "[codegen][project]") {
  rex::test::TempFolder folder;
  const auto result = RunProject(folder, kEntryBase, kDLLBase, kSecondDLLBase,
                                 R"([functions]
0x8F001008 = { name = "second_fn", size = 8 }
)");
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  CHECK(ReadTree(folder.path / "generated" / "b").find("DEFINE_REX_FUNC(second_fn)") !=
        std::string::npos);
  CHECK(ReadTree(folder.path / "generated" / "a").find("second_fn") == std::string::npos);
  CHECK(ReadTree(folder.path / "generated" / "entry").find("second_fn") == std::string::npos);
}

REX_TEST_CASE("A targeted run still rejects a shared address inside no image",
              "[codegen][project]") {
  rex::test::TempFolder folder;
  const auto result = RunProject(folder, kEntryBase, kDLLBase, kSecondDLLBase,
                                 R"([functions]
0x90001008 = { name = "lost_fn", size = 8 }
)",
                                 {}, {"a"});
  REQUIRE_FALSE(result);
  CHECK(result.error().message.find("0x90001008") != std::string::npos);
}

REX_TEST_CASE("A targeted run accepts a shared address inside an untargeted binary",
              "[codegen][project]") {
  rex::test::TempFolder folder;
  const auto result = RunProject(folder, kEntryBase, kDLLBase, kSecondDLLBase,
                                 R"([functions]
0x8F001008 = { name = "second_fn", size = 8 }
)",
                                 {}, {"a"});
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  CHECK_FALSE(fs::exists(folder.path / "generated" / "b"));
}

REX_TEST_CASE("Code generation resolves a DLL import against a module in another directory",
              "[codegen][project]") {
  constexpr u32 kExportTable = 0x3000;
  constexpr u32 kThunk = 0x1100;
  rex::test::TempFolder folder;
  WriteImage(folder.path / "entry.xex", kEntryBase, kLoadFirst);

  rex::test::XEXSpec exporter;
  exporter.load_address = kDLLBase;
  exporter.entry_point = kDLLBase + kCodeOffset;
  exporter.page_size_4kb = false;
  exporter.image = BuildImage(kDLLBase, kLoadFirst);
  exporter.export_table = kDLLBase + kExportTable;
  rex::test::PutBE32(exporter.image, kExportTable + 32, kDLLBase >> 16);
  rex::test::PutBE32(exporter.image, kExportTable + 36, 1);
  rex::test::PutBE32(exporter.image, kExportTable + 40, 1);
  rex::test::PutBE32(exporter.image, kExportTable + 44, kCodeOffset);
  const auto exporter_file = rex::test::BuildXEX(exporter);
  std::ofstream(folder.path / "Exporter.xex", std::ios::binary)
      .write(reinterpret_cast<const char*>(exporter_file.data()),
             static_cast<std::streamsize>(exporter_file.size()));

  rex::test::XEXSpec importer;
  importer.load_address = kSecondDLLBase;
  importer.entry_point = kSecondDLLBase + kCodeOffset;
  importer.page_size_4kb = false;
  importer.image = BuildImage(kSecondDLLBase, kLoadSecond);
  rex::test::PutBE32(importer.image, kThunk, (1u << 24) | 1);
  importer.imports.push_back({"exporter.xex", {kSecondDLLBase + kThunk}, 0});
  const auto importer_file = rex::test::BuildXEX(importer);
  fs::create_directories(folder.path / "sub");
  std::ofstream(folder.path / "sub" / "importer.xex", std::ios::binary)
      .write(reinterpret_cast<const char*>(importer_file.data()),
             static_cast<std::streamsize>(importer_file.size()));

  const auto manifest_path = folder.path / "demo_manifest.toml";
  std::ofstream(manifest_path) << R"([project]
name = "demo"
[entrypoint]
file_path = "entry.xex"
out_directory_path = "generated/entry"
[[modules]]
file_path = "sub/importer.xex"
guest_path = "sub/importer.xex"
out_directory_path = "generated/importer"
[[modules]]
file_path = "Exporter.xex"
guest_path = "Exporter.xex"
out_directory_path = "generated/exporter"
)";
  const auto manifest = rex::codegen::ManifestConfig::Load(manifest_path);
  REQUIRE(manifest);

  rex::testing::ReleaseTestMemory();
  rex::LogConfig config;
  config.debugger = false;
  config.categories.assign(rexglue::kLogCategories.begin(), rexglue::kLogCategories.end());
  REQUIRE(rex::log::Start(config).has_value());
  rex::codegen::ProjectRecompiler recompiler(*manifest);
  rex::codegen::ProjectRecompilerOptions options;
  options.force = true;
  const auto result = recompiler.Run(options);
  rex::log::Stop();
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
}
