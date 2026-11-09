/**
 * @file        tests/unit/kernel/exports_visibility_test.cpp
 * @brief       Symbols rexkernel exports
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <rex/kernel/shim.h>
#include <rex/platform/dynlib.h>
#include <rex/kernel/export.h>
#include <rex/platform.h>

#if REX_PLATFORM_MAC
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#endif

namespace {

using rex::kernel::ExportModule;
using rex::kernel::ExportType;

constexpr std::array<ExportModule, 3> kModules = {ExportModule::kXboxkrnl, ExportModule::kXAM,
                                                  ExportModule::kXBDM};

rex::platform::DynamicLibrary OpenKernel() {
  rex::platform::DynamicLibrary library;
  REQUIRE(library.Load(REX_TEST_KERNEL_PATH, rex::platform::SymbolResolution::kImmediate));
  return library;
}

std::vector<rex::u8> ReadLibraryFile(const char* path = REX_TEST_KERNEL_PATH) {
  std::ifstream file(path, std::ios::binary);
  REQUIRE(file);
  return std::vector<rex::u8>(std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>());
}

template <typename T>
T ReadAt(const std::vector<rex::u8>& bytes, size_t offset) {
  REQUIRE(offset + sizeof(T) <= bytes.size());
  T value;
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

std::string ReadCString(const std::vector<rex::u8>& bytes, size_t offset) {
  REQUIRE(offset < bytes.size());
  const char* begin = reinterpret_cast<const char*>(bytes.data()) + offset;
  const char* end = reinterpret_cast<const char*>(bytes.data()) + bytes.size();
  return std::string(begin, std::find(begin, end, '\0'));
}

#if REX_PLATFORM_WIN32

std::set<std::string> ListExportedNames(const std::vector<rex::u8>& bytes) {
  std::set<std::string> names;
  const size_t pe = ReadAt<rex::u32>(bytes, 0x3C);
  const rex::u16 section_count = ReadAt<rex::u16>(bytes, pe + 6);
  const rex::u16 optional_size = ReadAt<rex::u16>(bytes, pe + 20);
  const size_t optional = pe + 24;
  const rex::u32 export_rva = ReadAt<rex::u32>(bytes, optional + 112);
  if (export_rva == 0) {
    return names;
  }
  const size_t sections = optional + optional_size;
  const auto to_offset = [&](rex::u32 rva) -> size_t {
    for (rex::u16 i = 0; i < section_count; ++i) {
      const size_t entry = sections + static_cast<size_t>(i) * 40;
      const rex::u32 virtual_size = ReadAt<rex::u32>(bytes, entry + 8);
      const rex::u32 virtual_address = ReadAt<rex::u32>(bytes, entry + 12);
      const rex::u32 raw_pointer = ReadAt<rex::u32>(bytes, entry + 20);
      if (rva >= virtual_address && rva < virtual_address + virtual_size) {
        return raw_pointer + (rva - virtual_address);
      }
    }
    FAIL("export table is outside every section");
    return 0;
  };
  const size_t directory = to_offset(export_rva);
  const rex::u32 name_count = ReadAt<rex::u32>(bytes, directory + 24);
  const size_t name_table = to_offset(ReadAt<rex::u32>(bytes, directory + 32));
  for (rex::u32 i = 0; i < name_count; ++i) {
    names.insert(ReadCString(
        bytes, to_offset(ReadAt<rex::u32>(bytes, name_table + static_cast<size_t>(i) * 4))));
  }
  return names;
}

#elif REX_PLATFORM_MAC

std::set<std::string> ListExportedNames(const std::vector<rex::u8>& bytes) {
  std::set<std::string> names;
  const mach_header_64 header = ReadAt<mach_header_64>(bytes, 0);
  REQUIRE(header.magic == MH_MAGIC_64);
  symtab_command symbols{};
  dysymtab_command dynamic_symbols{};
  size_t offset = sizeof(mach_header_64);
  for (rex::u32 i = 0; i < header.ncmds; ++i) {
    const load_command command = ReadAt<load_command>(bytes, offset);
    if (command.cmd == LC_SYMTAB) {
      symbols = ReadAt<symtab_command>(bytes, offset);
    } else if (command.cmd == LC_DYSYMTAB) {
      dynamic_symbols = ReadAt<dysymtab_command>(bytes, offset);
    }
    offset += command.cmdsize;
  }
  // Hidden symbols are local after the link, so the defined external symbols are the exports.
  for (rex::u32 i = 0; i < dynamic_symbols.nextdefsym; ++i) {
    const size_t index = size_t{dynamic_symbols.iextdefsym} + i;
    const nlist_64 symbol = ReadAt<nlist_64>(bytes, symbols.symoff + index * sizeof(nlist_64));
    names.insert(ReadCString(bytes, symbols.stroff + symbol.n_un.n_strx).substr(1));
  }
  return names;
}

#else

std::set<std::string> ListExportedNames(const std::vector<rex::u8>& bytes) {
  std::set<std::string> names;
  const rex::u64 section_table = ReadAt<rex::u64>(bytes, 0x28);
  const rex::u16 section_size = ReadAt<rex::u16>(bytes, 0x3A);
  const rex::u16 section_count = ReadAt<rex::u16>(bytes, 0x3C);
  for (rex::u16 i = 0; i < section_count; ++i) {
    const size_t section = section_table + static_cast<size_t>(i) * section_size;
    constexpr rex::u32 kDynamicSymbols = 11;
    if (ReadAt<rex::u32>(bytes, section + 4) != kDynamicSymbols) {
      continue;
    }
    const rex::u64 symbol_offset = ReadAt<rex::u64>(bytes, section + 24);
    const rex::u64 symbol_bytes = ReadAt<rex::u64>(bytes, section + 32);
    const rex::u32 string_section = ReadAt<rex::u32>(bytes, section + 40);
    const size_t strings = section_table + static_cast<size_t>(string_section) * section_size;
    const rex::u64 string_offset = ReadAt<rex::u64>(bytes, strings + 24);
    for (rex::u64 offset = 0; offset + 24 <= symbol_bytes; offset += 24) {
      const size_t symbol = symbol_offset + offset;
      const rex::u32 name_index = ReadAt<rex::u32>(bytes, symbol);
      const rex::u8 other = ReadAt<rex::u8>(bytes, symbol + 5);
      const rex::u16 section_index = ReadAt<rex::u16>(bytes, symbol + 6);
      if (section_index == 0 || (other & 0x3) != 0) {
        continue;
      }
      names.insert(ReadCString(bytes, string_offset + name_index));
    }
  }
  return names;
}

#endif

}  // namespace

REX_TEST_CASE("The kernel library exports every function row of the export tables",
              "[kernel][exports]") {
  const auto library = OpenKernel();
  rex::u32 resolved = 0;
  for (const auto module : kModules) {
    for (const auto& entry : rex::kernel::GetExports(module)) {
      if (entry.type != ExportType::kFunction) {
        continue;
      }
      const std::string symbol = "__imp__" + std::string(entry.name);
      INFO(symbol);
      CHECK(library.GetRawSymbol(symbol.c_str()) != nullptr);
      ++resolved;
    }
  }
  CHECK(resolved > 0);
}

REX_TEST_CASE("The kernel library exports no __imp__ symbol outside the export tables",
              "[kernel][exports]") {
  std::set<std::string> expected;
  for (const auto module : kModules) {
    for (const auto& entry : rex::kernel::GetExports(module)) {
      expected.insert("__imp__" + std::string(entry.name));
    }
  }
  for (const auto& entry : rex::kernel::shim::kRegistry) {
    expected.insert("__imp__" + std::string(entry.name));
  }
  const std::set<std::string> exported = ListExportedNames(ReadLibraryFile());
  rex::u32 seen = 0;
  for (const std::string& name : exported) {
    if (!name.starts_with("__imp__")) {
      continue;
    }
    INFO(name);
    CHECK(expected.contains(name));
    ++seen;
  }
  CHECK(seen > 0);
}

REX_TEST_CASE("The kernel library exports every shim", "[kernel][exports]") {
  const auto library = OpenKernel();
  for (const auto& entry : rex::kernel::shim::kRegistry) {
    const std::string symbol = "rexshim_" + std::string(entry.name);
    INFO(symbol);
    CHECK(library.GetRawSymbol(symbol.c_str()) != nullptr);
  }
}

REX_TEST_CASE("The kernel library exports the module entry points", "[kernel][exports]") {
  const auto library = OpenKernel();
  CHECK(library.GetRawSymbol("ReXCreateInterface") != nullptr);
  CHECK(library.GetRawSymbol("ReXEnumerateInterfaces") != nullptr);
  CHECK(library.GetRawSymbol("ReXBuildIdentity") != nullptr);
}

REX_TEST_CASE("The kernel library exports none of its third party code", "[kernel][exports]") {
  const auto library = OpenKernel();
  CHECK(library.GetRawSymbol("stbi_load_from_memory") == nullptr);
}

REX_TEST_CASE("The XAM library exports no entry point symbol", "[kernel][exports][xam]") {
  const std::set<std::string> names = ListExportedNames(ReadLibraryFile(REX_TEST_XAM_PATH));
  REQUIRE_FALSE(names.empty());
  for (const std::string& name : names) {
    INFO(name);
    CHECK_FALSE(name.starts_with("__imp__"));
    CHECK_FALSE(name.starts_with("rexshim_"));
  }
}

REX_TEST_CASE("The XBDM library exports no entry point symbol", "[kernel][exports][xbdm]") {
  const std::set<std::string> names = ListExportedNames(ReadLibraryFile(REX_TEST_XBDM_PATH));
  REQUIRE_FALSE(names.empty());
  for (const std::string& name : names) {
    INFO(name);
    CHECK_FALSE(name.starts_with("__imp__"));
    CHECK_FALSE(name.starts_with("rexshim_"));
  }
}
