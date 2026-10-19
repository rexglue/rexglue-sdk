/**
 * @file        tests/code_image.h
 * @brief       Builds a single module code image and runs code generation over it
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "rex_test.h"
#include "temp_folder.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include <rex/codegen/manifest.h>
#include <rex/codegen/project_recompiler.h>
#include <rex/filesystem/xex.h>
#include <rex/logging/config.h>
#include <rex/logging/logger.h>
#include <rex/logging/sink.h>

#include "core/xex_builder.h"
#include "pe/pe_image.h"
#include "rexglue/log_categories.h"
#include "test_memory.h"

namespace rex::test {

inline constexpr u32 kCodeImageBase = 0x82000000;
inline constexpr u32 kCodeImageSize = 0x10000;
inline constexpr u32 kCodeImageCodeOffset = 0x1000;
inline constexpr u32 kCodeImagePDataOffset = 0x2000;
inline constexpr u32 kCodeImageRDataOffset = 0x3000;
inline constexpr u32 kPDataSaveMillicode = 0u << 30;
inline constexpr u32 kPDataNoHandler = 1u << 30;
inline constexpr u32 kPDataRestoreMillicode = 2u << 30;
inline constexpr u32 kPDataHandler = 3u << 30;

/**
 * One exception directory entry.
 */
struct PDataEntry {
  u32 begin;                   /**< Address of the first instruction. */
  u32 length;                  /**< Instruction count including the prologue. */
  u32 prolog = 0;              /**< Instruction count of the prologue. */
  u32 flags = kPDataNoHandler; /**< Flag bits of the entry. */
};

/**
 * Builds a PE image with a code section at kCodeImageCodeOffset and an exception directory.
 *
 * @param words The instruction words of the code section.
 * @param pdata The exception directory entries, at least 1.
 * @param rdata The words of a read only data section at kCodeImageRDataOffset, none to omit it.
 * @return The image bytes.
 */
inline std::vector<u8> BuildCodeImage(std::span<const u32> words, std::span<const PDataEntry> pdata,
                                      std::span<const u32> rdata = {}) {
  std::vector<u8> image(kCodeImageSize, 0);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
  dos->e_magic = IMAGE_DOS_SIGNATURE;
  dos->e_cblp = 0x90;
  dos->e_lfanew = 0x80;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(image.data() + 0x80);
  nt->Signature = IMAGE_NT_SIGNATURE;
  nt->FileHeader.Machine = IMAGE_FILE_MACHINE_POWERPCBE;
  nt->FileHeader.NumberOfSections = rdata.empty() ? 2 : 3;
  nt->FileHeader.SizeOfOptionalHeader = IMAGE_SIZEOF_NT_OPTIONAL_HEADER;
  nt->FileHeader.Characteristics = IMAGE_FILE_32BIT_MACHINE;
  nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
  nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_XBOX;
  nt->OptionalHeader.SizeOfHeaders = kCodeImageCodeOffset;
  nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress =
      kCodeImagePDataOffset;
  nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size =
      static_cast<u32>(pdata.size() * 8);
  auto* section = reinterpret_cast<IMAGE_SECTION_HEADER*>(image.data() + 0x80 + 24 +
                                                          IMAGE_SIZEOF_NT_OPTIONAL_HEADER);
  std::memcpy(section->Name, ".text", 5);
  section->VirtualAddress = kCodeImageCodeOffset;
  section->Misc.VirtualSize = std::max<u32>(0x200, static_cast<u32>(words.size() * 4));
  section->Characteristics = rex::filesystem::kPESectionMemoryExecute;
  auto* pdata_section = section + 1;
  std::memcpy(pdata_section->Name, ".pdata", 6);
  pdata_section->VirtualAddress = kCodeImagePDataOffset;
  pdata_section->Misc.VirtualSize = 0x100;
  if (!rdata.empty()) {
    auto* rdata_section = pdata_section + 1;
    std::memcpy(rdata_section->Name, ".rdata", 6);
    rdata_section->VirtualAddress = kCodeImageRDataOffset;
    rdata_section->Misc.VirtualSize = std::max<u32>(0x100, static_cast<u32>(rdata.size() * 4));
    u32 rdata_offset = kCodeImageRDataOffset;
    for (u32 word : rdata) {
      PutBE32(image, rdata_offset, word);
      rdata_offset += 4;
    }
  }
  u32 offset = kCodeImagePDataOffset;
  for (const auto& entry : pdata) {
    PutBE32(image, offset, entry.begin);
    PutBE32(image, offset + 4, entry.prolog | (entry.length << 8) | entry.flags);
    offset += 8;
  }
  offset = kCodeImageCodeOffset;
  for (u32 word : words) {
    PutBE32(image, offset, word);
    offset += 4;
  }
  return image;
}

/**
 * Concatenates every file under a folder.
 *
 * @param root The folder.
 * @return The file contents.
 */
inline std::string ReadGeneratedTree(const std::filesystem::path& root) {
  std::string text;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::ifstream in(entry.path(), std::ios::binary);
    text.append(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  return text;
}

/**
 * Runs code generation over an image as the entrypoint of a project.
 *
 * @param folder The folder that holds the project and receives the output.
 * @param image The image bytes.
 * @param extra_manifest Text appended to the manifest.
 * @param force True to continue past validation errors, as the force option does.
 * @param imports The import libraries of the image and the thunk addresses of each.
 * @param sink A sink that receives the log messages of the run, or null for none.
 * @return The result of the run, with the output under folder/generated/entry.
 */
inline Result<void> GenerateFromImage(const TempFolder& folder, std::vector<u8> image,
                                      std::string_view extra_manifest = {}, bool force = true,
                                      std::vector<XEXImportSpec> imports = {},
                                      log::ISink* sink = nullptr) {
  XEXSpec spec;
  spec.load_address = kCodeImageBase;
  spec.entry_point = kCodeImageBase + kCodeImageCodeOffset;
  spec.page_size_4kb = false;
  spec.image = std::move(image);
  spec.imports = std::move(imports);
  const auto file = BuildXEX(spec);
  std::ofstream(folder.path / "entry.xex", std::ios::binary)
      .write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
  const auto manifest_path = folder.path / "demo_manifest.toml";
  std::ofstream(manifest_path) << R"([project]
name = "demo"
[entrypoint]
file_path = "entry.xex"
out_directory_path = "generated/entry"
)" << extra_manifest;
  const auto manifest = codegen::ManifestConfig::Load(manifest_path);
  if (!manifest) {
    return Err<void>(ErrorCategory::kConfig, "the manifest did not load");
  }

  test::ReleaseTestMemory();
  LogConfig config;
  config.debugger = false;
  if (sink) {
    config.sinks.push_back(sink);
  }
  config.categories.assign(rexglue::kLogCategories.begin(), rexglue::kLogCategories.end());
  REQUIRE(log::Start(config).has_value());
  codegen::ProjectRecompiler recompiler(*manifest);
  codegen::ProjectRecompilerOptions options;
  options.force = force;
  auto result = recompiler.Run(options);
  log::Stop();
  return result;
}

}  // namespace rex::test
