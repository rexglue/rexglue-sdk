/**
 * @file        tests/unit/core/core_fixes_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdarg>
#include <string>
#include <vector>

#include <rex/bit.h>
#include <rex/string/buffer.h>
#include <rex/string/utf8.h>
#include <rex/types.h>

namespace {

std::string Format(const char* format, ...) {
  rex::StringBuffer buffer;
  va_list args;
  va_start(args, format);
  buffer.AppendVarargs(format, args);
  va_end(args);
  return buffer.to_string();
}

}  // namespace

TEST_CASE("BitMap copies every source word", "[bit]") {
  rex::u64 words[3] = {0x1111111111111111, 0x2222222222222222, 0x3333333333333333};
  rex::BitMap map(words, sizeof(words) * 8);
  REQUIRE(map.data() == std::vector<rex::u64>{words[0], words[1], words[2]});
}

TEST_CASE("AppendVarargs formats every argument", "[string]") {
  REQUIRE(Format("%s-%d-%s", "alpha", 42, "beta") == "alpha-42-beta");
}

TEST_CASE("utf8_upper_ascii uppercases ASCII letters", "[string]") {
  REQUIRE(rex::utf8_upper_ascii("abc XYZ 1") == "ABC XYZ 1");
}

TEST_CASE("utf8_ends_with_case ignores ASCII case", "[string]") {
  REQUIRE(rex::utf8_ends_with_case("Default.XEX", ".xex"));
  REQUIRE(rex::utf8_ends_with_case("a", "A"));
  REQUIRE_FALSE(rex::utf8_ends_with_case("default.xex", ".xexp"));
  REQUIRE_FALSE(rex::utf8_ends_with_case("x", "xx"));
  REQUIRE(rex::utf8_ends_with_case("abc", ""));
}
