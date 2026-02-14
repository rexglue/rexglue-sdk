/**
 * @file        runtime/map_parser.h
 * @brief       Utilities for parsing symbol map files
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rex/system/binary_types.h>

namespace rex::runtime {

/**
 * Options for parsing map files.
 */
struct MapParseOptions {
  /// Base address to add to all symbol addresses (for relative maps)
  uint32_t base_address = 0;

  /// If true, only parse symbols that look like functions
  bool functions_only = false;

  /// If non-empty, only include symbols whose names start with this prefix
  std::string_view prefix_filter;
};

/**
 * Error types for map parsing operations.
 */
enum class MapParseError { FileNotFound, FileReadError, InvalidFormat, Empty };

/**
 * Get a human-readable string for a MapParseError.
 */
constexpr std::string_view to_string(MapParseError error) {
  switch (error) {
    case MapParseError::FileNotFound:
      return "File not found";
    case MapParseError::FileReadError:
      return "Failed to read file";
    case MapParseError::InvalidFormat:
      return "Invalid map format";
    case MapParseError::Empty:
      return "No symbols found";
  }
  return "Unknown error";
}

/**
 * Parse an nm-style map file (format: "address type name").
 *
 * This is the format produced by `nm` on Unix systems:
 *   00000000 t test_add1
 *   00000010 T test_sub1
 *
 * Symbol types:
 *   T/t = text (code)
 *   D/d = data
 *   B/b = bss
 *   U   = undefined
 *
 * @param map_path    Path to the .map file
 * @param options     Parsing options (base address, filters)
 * @return            Vector of parsed symbols or error
 */
std::expected<std::vector<BinarySymbol>, MapParseError> ParseNmMap(
    const std::filesystem::path& map_path, const MapParseOptions& options = {});

/**
 * Parse nm-style map data from a string.
 *
 * @param map_data    Map file contents as a string
 * @param options     Parsing options
 * @return            Vector of parsed symbols or error
 */
std::expected<std::vector<BinarySymbol>, MapParseError> ParseNmMapString(
    std::string_view map_data, const MapParseOptions& options = {});

}  // namespace rex::runtime
