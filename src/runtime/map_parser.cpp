#include <charconv>
#include <format>

#include <rex/filesystem/error.h>
#include <rex/platform/file.h>
#include <rex/runtime/map_parser.h>

namespace rex::runtime {

namespace {

/**
 * Determines the symbol type from an nm type character.
 *
 * @param c The nm type character.
 * @return Function for T and t, Data for D, d, B, b, R and r, Import for U, otherwise Unknown.
 */
BinarySymbolType SymbolTypeFromNmChar(char c) {
  switch (c) {
    case 'T':
    case 't':
      return BinarySymbolType::kFunction;
    case 'D':
    case 'd':
    case 'B':
    case 'b':
    case 'R':
    case 'r':
      return BinarySymbolType::kData;
    case 'U':
      return BinarySymbolType::kImport;
    default:
      return BinarySymbolType::kUnknown;
  }
}

/**
 * Reports whether an nm type character marks a function.
 *
 * @param c The nm type character.
 * @return True when the character is T or t.
 */
bool IsFunctionType(char c) {
  return c == 'T' || c == 't';
}

}  // namespace

Result<std::vector<BinarySymbol>> ParseNmMapString(std::string_view map_data,
                                                   const MapParseOptions& options) {
  std::vector<BinarySymbol> symbols;

  size_t pos = 0;
  while (pos < map_data.size()) {
    size_t eol = map_data.find('\n', pos);
    if (eol == std::string_view::npos) {
      eol = map_data.size();
    }

    std::string_view line = map_data.substr(pos, eol - pos);
    pos = eol + 1;

    if (line.empty())
      continue;

    if (!line.empty() && line.back() == '\r') {
      line = line.substr(0, line.size() - 1);
    }

    // nm format is "address type name", for example "00000000 t test_add1".
    size_t space1 = line.find(' ');
    if (space1 == std::string_view::npos)
      continue;

    std::string_view addr_str = line.substr(0, space1);
    u32 address = 0;
    auto [ptr, ec] =
        std::from_chars(addr_str.data(), addr_str.data() + addr_str.size(), address, 16);
    if (ec != std::errc())
      continue;

    size_t type_pos = space1 + 1;
    while (type_pos < line.size() && line[type_pos] == ' ') {
      ++type_pos;
    }
    if (type_pos >= line.size())
      continue;

    char type_char = line[type_pos];

    size_t name_pos = type_pos + 1;
    while (name_pos < line.size() && line[name_pos] == ' ') {
      ++name_pos;
    }
    if (name_pos >= line.size())
      continue;

    std::string_view name = line.substr(name_pos);

    if (options.functions_only && !IsFunctionType(type_char)) {
      continue;
    }

    if (!options.prefix_filter.empty()) {
      if (!name.starts_with(options.prefix_filter)) {
        continue;
      }
    }

    BinarySymbol sym;
    sym.name = std::string(name);
    sym.address = address + options.base_address;
    sym.size = 0;
    sym.type = SymbolTypeFromNmChar(type_char);

    symbols.push_back(std::move(sym));
  }

  if (symbols.empty()) {
    return Err<std::vector<BinarySymbol>>(filesystem::BuildError(
        filesystem::FileSystemError::kCorrupt, "no symbols found in the map"));
  }

  return symbols;
}

Result<std::vector<BinarySymbol>> ParseNmMap(const std::filesystem::path& map_path,
                                             const MapParseOptions& options) {
  if (!std::filesystem::exists(map_path)) {
    return Err<std::vector<BinarySymbol>>(
        filesystem::BuildError(filesystem::FileSystemError::kNotFound,
                               std::format("map file {} not found", map_path.string())));
  }

  auto bytes = platform::ReadHostFile(map_path);
  if (!bytes) {
    return Err<std::vector<BinarySymbol>>(
        filesystem::BuildError(filesystem::FileSystemError::kIO,
                               std::format("cannot read map file {}", map_path.string())));
  }

  return ParseNmMapString(std::string(bytes->begin(), bytes->end()), options);
}

}  // namespace rex::runtime
