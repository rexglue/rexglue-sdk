/**
 * @file        tests/unit/runtime/map_parser_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/filesystem/error.h>
#include <rex/runtime/map_parser.h>

#include "file_text.h"
#include "temp_folder.h"

using rex::filesystem::FileSystemError;
using rex::runtime::MapParseOptions;
using rex::runtime::ParseNmMap;
using rex::runtime::ParseNmMapString;

REX_TEST_CASE("A map string yields its symbols with the base address added", "[runtime][map]") {
  MapParseOptions options;
  options.base_address = 0x1000;
  const auto symbols = ParseNmMapString("00000000 t first\r\n00000010 T second\n", options);
  REQUIRE(symbols);
  REQUIRE(symbols->size() == 2);
  CHECK((*symbols)[0].name == "first");
  CHECK((*symbols)[1].address == 0x1010);
}

REX_TEST_CASE("A map string without symbols is a corrupt map error", "[runtime][map]") {
  const auto symbols = ParseNmMapString("not a map line\n");
  REQUIRE_FALSE(symbols);
  CHECK(symbols.error().category == rex::ErrorCategory::kFormat);
}

REX_TEST_CASE("A missing map file is a not found error", "[runtime][map]") {
  const rex::test::TempFolder folder("rex_map_test_");
  const auto symbols = ParseNmMap(folder.path / "absent.map");
  REQUIRE_FALSE(symbols);
  CHECK(symbols.error().category == rex::ErrorCategory::kNotFound);
}

REX_TEST_CASE("A map file is read from disk", "[runtime][map]") {
  const rex::test::TempFolder folder("rex_map_test_");
  rex::test::WriteText(folder.path / "a.map", "00000020 T entry\n");
  const auto symbols = ParseNmMap(folder.path / "a.map");
  REQUIRE(symbols);
  CHECK(symbols->size() == 1);
}
