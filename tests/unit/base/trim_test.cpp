/**
 * @file        tests/unit/base/trim_test.cpp
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

#include <rex/string/text.h>

REX_TEST_CASE("Trim removes space, tab, carriage return and line feed from narrow text",
              "[base][string]") {
  using namespace std::string_view_literals;
  CHECK(rex::string::Trim(" \t\r\nkey = value \t\r\n"sv) == "key = value"sv);
  CHECK(rex::string::TrimLeft(" \t\r\nabc \r\n"sv) == "abc \r\n"sv);
  CHECK(rex::string::TrimRight(" \r\nabc \t\r\n"sv) == " \r\nabc"sv);
  CHECK(rex::string::Trim("   "sv).empty());
  CHECK(rex::string::Trim(""sv).empty());
  CHECK(rex::string::Trim("xxabcxx"sv, "x") == "abc"sv);
  CHECK(rex::string::TrimString(std::string("  a b  ")) == "a b");
}

REX_TEST_CASE("Trim takes UTF-16 text", "[base][string]") {
  using namespace std::string_view_literals;
  CHECK(rex::string::Trim(u" \t\r\nkey = value \t\r\n"sv) == u"key = value"sv);
  CHECK(rex::string::TrimLeft(u" \t\r\nabc \r\n"sv) == u"abc \r\n"sv);
  CHECK(rex::string::TrimRight(u" \r\nabc \t\r\n"sv) == u" \r\nabc"sv);
  CHECK(rex::string::Trim(u"   "sv).empty());
  CHECK(rex::string::Trim(u""sv).empty());
  CHECK(rex::string::Trim(u"xxabcxx"sv, u"x"sv) == u"abc"sv);
  CHECK(rex::string::Trim(u"\u00A0abc\u00A0"sv) == u"\u00A0abc\u00A0"sv);
}

REX_TEST_CASE("Trim takes wide text", "[base][string]") {
  using namespace std::string_view_literals;
  CHECK(rex::string::Trim(L" \tabc\r\n"sv) == L"abc"sv);
  CHECK(rex::string::kWhitespaceOf<char16_t> == u" \t\r\n"sv);
}
