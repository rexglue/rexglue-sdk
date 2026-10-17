/**
 * @file        tests/unit/core/trim_test.cpp
 * @brief       Trimming of narrow and wide text
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <string>
#include <string_view>

#include <rex/string.h>

REX_TEST_CASE("Trim removes space, tab, carriage return and line feed from narrow text",
              "[core][string]") {
  using namespace std::string_view_literals;
  CHECK(rex::Trim(" \t\r\nkey = value \t\r\n"sv) == "key = value"sv);
  CHECK(rex::TrimLeft(" \t\r\nabc \r\n"sv) == "abc \r\n"sv);
  CHECK(rex::TrimRight(" \r\nabc \t\r\n"sv) == " \r\nabc"sv);
  CHECK(rex::Trim("   "sv).empty());
  CHECK(rex::Trim(""sv).empty());
  CHECK(rex::Trim("xxabcxx"sv, "x") == "abc"sv);
  CHECK(rex::TrimString(std::string("  a b  ")) == "a b");
}

REX_TEST_CASE("Trim takes UTF-16 text", "[core][string]") {
  using namespace std::string_view_literals;
  CHECK(rex::Trim(u" \t\r\nkey = value \t\r\n"sv) == u"key = value"sv);
  CHECK(rex::TrimLeft(u" \t\r\nabc \r\n"sv) == u"abc \r\n"sv);
  CHECK(rex::TrimRight(u" \r\nabc \t\r\n"sv) == u" \r\nabc"sv);
  CHECK(rex::Trim(u"   "sv).empty());
  CHECK(rex::Trim(u""sv).empty());
  CHECK(rex::Trim(u"xxabcxx"sv, u"x"sv) == u"abc"sv);
  CHECK(rex::Trim(u"\u00A0abc\u00A0"sv) == u"\u00A0abc\u00A0"sv);
}

REX_TEST_CASE("Trim takes wide text", "[core][string]") {
  using namespace std::string_view_literals;
  CHECK(rex::Trim(L" \tabc\r\n"sv) == L"abc"sv);
  CHECK(rex::kWhitespaceOf<char16_t> == u" \t\r\n"sv);
}
