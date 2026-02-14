/**
 * @file        runtime/map_parser.cpp
 * @brief       Implementation of map file parsing utilities
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <charconv>
#include <fstream>
#include <sstream>

#include <rex/system/map_parser.h>

namespace rex::runtime {

namespace {

/**
 * Determine symbol type from nm symbol type character.
 */
BinarySymbolType SymbolTypeFromNmChar(char c) {
  switch (c) {
    case 'T':
    case 't':
      return BinarySymbolType::Function;
    case 'D':
    case 'd':
    case 'B':
    case 'b':
    case 'R':
    case 'r':
      return BinarySymbolType::Data;
    case 'U':
      return BinarySymbolType::Import;
    default:
      return BinarySymbolType::Unknown;
  }
}

/**
 * Check if nm symbol type indicates a function.
 */
bool IsFunctionType(char c) {
  return c == 'T' || c == 't';
}

}  // namespace

std::expected<std::vector<BinarySymbol>, MapParseError> ParseNmMapString(
    std::string_view map_data, const MapParseOptions& options) {
  std::vector<BinarySymbol> symbols;

  // Parse line by line
  size_t pos = 0;
  while (pos < map_data.size()) {
    // Find end of line
    size_t eol = map_data.find('\n', pos);
    if (eol == std::string_view::npos) {
      eol = map_data.size();
    }

    std::string_view line = map_data.substr(pos, eol - pos);
    pos = eol + 1;

    // Skip empty lines
    if (line.empty())
      continue;

    // Remove trailing \r if present
    if (!line.empty() && line.back() == '\r') {
      line = line.substr(0, line.size() - 1);
    }

    // nm format: "address type name" (space-separated)
    // Example: "00000000 t test_add1"

    // Find first space (after address)
    size_t space1 = line.find(' ');
    if (space1 == std::string_view::npos)
      continue;

    // Parse address
    std::string_view addr_str = line.substr(0, space1);
    uint32_t address = 0;
    auto [ptr, ec] =
        std::from_chars(addr_str.data(), addr_str.data() + addr_str.size(), address, 16);
    if (ec != std::errc())
      continue;

    // Skip whitespace to find type
    size_t type_pos = space1 + 1;
    while (type_pos < line.size() && line[type_pos] == ' ') {
      ++type_pos;
    }
    if (type_pos >= line.size())
      continue;

    char type_char = line[type_pos];

    // Skip to name
    size_t name_pos = type_pos + 1;
    while (name_pos < line.size() && line[name_pos] == ' ') {
      ++name_pos;
    }
    if (name_pos >= line.size())
      continue;

    std::string_view name = line.substr(name_pos);

    // Apply filters
    if (options.functions_only && !IsFunctionType(type_char)) {
      continue;
    }

    if (!options.prefix_filter.empty()) {
      if (!name.starts_with(options.prefix_filter)) {
        continue;
      }
    }

    // Create symbol
    BinarySymbol sym;
    sym.name = std::string(name);
    sym.address = address + options.base_address;
    sym.size = 0;  // nm doesn't provide size
    sym.type = SymbolTypeFromNmChar(type_char);

    symbols.push_back(std::move(sym));
  }

  if (symbols.empty()) {
    return std::unexpected(MapParseError::Empty);
  }

  return symbols;
}

std::expected<std::vector<BinarySymbol>, MapParseError> ParseNmMap(
    const std::filesystem::path& map_path, const MapParseOptions& options) {
  // Check if file exists
  if (!std::filesystem::exists(map_path)) {
    return std::unexpected(MapParseError::FileNotFound);
  }

  // Read file contents
  std::ifstream file(map_path, std::ios::binary);
  if (!file) {
    return std::unexpected(MapParseError::FileReadError);
  }

  std::ostringstream ss;
  ss << file.rdbuf();
  std::string contents = ss.str();

  return ParseNmMapString(contents, options);
}

}  // namespace rex::runtime
