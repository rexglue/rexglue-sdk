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
#include <vector>

#include <rex/codegen/manifest.h>
#include <rex/codegen/project_recompiler.h>
#include <rex/filesystem/xex.h>
#include <rex/logging.h>

#include "core/xex_builder.h"
#include "pe/pe_image.h"
#include "rexglue/log_categories.h"
#include "test_memory.h"

namespace fs = std::filesystem;

namespace {

constexpr u32 kEntryBase = 0x82000000;
constexpr u32 kDLLBase = 0x8E000000;
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
                             u32 second_base) {
  WriteImage(folder.path / "entry.xex", entry_base, kLoadFirst);
  WriteImage(folder.path / "a.xex", first_base, kLoadFirst);
  WriteImage(folder.path / "b.xex", second_base, kLoadSecond);
  const auto manifest_path = folder.path / "demo_manifest.toml";
  std::ofstream(manifest_path) << R"([project]
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
