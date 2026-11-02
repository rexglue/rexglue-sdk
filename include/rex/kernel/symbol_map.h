/**
 * @file        kernel/symbol_map.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <filesystem>
#include <string_view>
#include <vector>

#include <rex/types.h>
#include <rex/platform.h>
#include <rex/result.h>

namespace rex::kernel {

enum class BinarySymbolType : u8 { kFunction, kData, kImport, kUnknown };

struct BinarySymbol {
  std::string name;
  u32 address = 0;
  u32 size = 0;
  BinarySymbolType type = BinarySymbolType::kUnknown;
};

struct BinarySection {
  std::string name;
  u32 virtual_address = 0; /**< Guest address of the first byte. */
  u32 virtual_size = 0;
  const u8* host_data = nullptr; /**< Host address of the first byte. */
  bool executable = false;
  bool writable = false;
};

}  // namespace rex::kernel

namespace rex::kernel {

struct MapParseOptions {
  /** Base address added to all symbol addresses, for relative maps. */
  u32 base_address = 0;

  /** True to parse only symbols that look like functions. */
  bool functions_only = false;

  /** When non empty, only symbols whose names start with this prefix are included. */
  std::string_view prefix_filter;
};

/**
 * Parses a map file in the nm format whose lines read "address type name", such as
 * "00000010 T test_sub1". Types are T/t code, D/d data, B/b bss and U undefined.
 *
 * @param options The base address and filters.
 * @return The symbols, or a not found, IO or corrupt map error.
 */
[[nodiscard]] REX_KERNEL_API Result<std::vector<BinarySymbol>> ParseNmMap(
    const std::filesystem::path& map_path, const MapParseOptions& options = {});

/**
 * Parses map data in the nm format from a string.
 *
 * @param options The base address and filters.
 * @return The symbols, or a not found, IO or corrupt map error.
 */
[[nodiscard]] REX_KERNEL_API Result<std::vector<BinarySymbol>> ParseNmMapString(
    std::string_view map_data, const MapParseOptions& options = {});

}  // namespace rex::kernel
