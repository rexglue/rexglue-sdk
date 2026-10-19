/**
 * @file        runtime/map_parser.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/export.h>
#include <rex/runtime/binary_types.h>

namespace rex::runtime {

/**
 * Options for parsing map files.
 */
struct MapParseOptions {
  /** Base address added to all symbol addresses, for relative maps. */
  u32 base_address = 0;

  /** True to parse only symbols that look like functions. */
  bool functions_only = false;

  /** When non empty, only symbols whose names start with this prefix are included. */
  std::string_view prefix_filter;
};

/**
 * Parses a map file in the nm format whose lines read "address type name".
 *
 * This is the format produced by `nm` on Unix systems.
 * @code
 * 00000000 t test_add1
 * 00000010 T test_sub1
 * @endcode
 *
 * Symbol types.
 *   T/t = text (code)
 *   D/d = data
 *   B/b = bss
 *   U   = undefined
 *
 * @param map_path    Path to the .map file
 * @param options     Parsing options (base address, filters)
 * @return            Vector of parsed symbols, or a not found, IO or corrupt map error
 */
[[nodiscard]] REX_RUNTIME_API Result<std::vector<BinarySymbol>> ParseNmMap(
    const std::filesystem::path& map_path, const MapParseOptions& options = {});

/**
 * Parses map data in the nm format from a string.
 *
 * @param map_data    Map file contents as a string
 * @param options     Parsing options
 * @return            Vector of parsed symbols, or a not found, IO or corrupt map error
 */
[[nodiscard]] REX_RUNTIME_API Result<std::vector<BinarySymbol>> ParseNmMapString(
    std::string_view map_data, const MapParseOptions& options = {});

}  // namespace rex::runtime
