/**
 * @file        tests/unit/kernel/title_image.h
 * @brief       A minimal title on disk and the recompiled image with its code and image ranges
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <rex/ppc/image.h>
#include <rex/filesystem/xex.h>

#include "base/xex_builder.h"
#include "pe/pe_image.h"

namespace rex::test {

constexpr u32 kTitleImageBase = 0x82000000;
constexpr u32 kTitleEntry = 0x82001000;
constexpr u32 kTitleId = 0x4D5307E6;

inline std::vector<u8> BuildTitleImage() {
  std::vector<u8> image(0x2000, 0);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
  dos->e_magic = IMAGE_DOS_SIGNATURE;
  dos->e_cblp = 0x90;
  dos->e_lfanew = 0x80;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(image.data() + 0x80);
  nt->Signature = IMAGE_NT_SIGNATURE;
  nt->FileHeader.Machine = IMAGE_FILE_MACHINE_POWERPCBE;
  nt->FileHeader.NumberOfSections = 1;
  nt->FileHeader.SizeOfOptionalHeader = IMAGE_SIZEOF_NT_OPTIONAL_HEADER;
  nt->FileHeader.Characteristics = IMAGE_FILE_32BIT_MACHINE;
  nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
  nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_XBOX;
  nt->OptionalHeader.SizeOfHeaders = 0x1000;
  auto* section = reinterpret_cast<IMAGE_SECTION_HEADER*>(image.data() + 0x80 + 24 +
                                                          IMAGE_SIZEOF_NT_OPTIONAL_HEADER);
  std::memcpy(section->Name, ".text", 5);
  section->VirtualAddress = 0x1000;
  section->Misc.VirtualSize = 0x1000;
  section->Characteristics = rex::filesystem::kPESectionMemoryExecute;
  return image;
}

inline void WriteTitle(const std::filesystem::path& game_root) {
  XEXSpec spec;
  spec.load_address = kTitleImageBase;
  spec.entry_point = kTitleEntry;
  spec.title_id = kTitleId;
  spec.image = BuildTitleImage();
  const auto file = BuildXEX(spec);
  std::filesystem::create_directories(game_root);
  std::ofstream(game_root / "default.xex", std::ios::binary)
      .write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
}

inline ppc::PPCImage MakeTitleImage(const PPCFuncMapping* mappings = nullptr) {
  ppc::PPCImage image{};
  image.code_base = kTitleEntry;
  image.code_size = 0x1000;
  image.image_base = kTitleImageBase;
  image.image_size = 0x10000;
  image.func_mappings = mappings;
  return image;
}

}  // namespace rex::test
