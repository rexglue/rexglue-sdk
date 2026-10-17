/**
 * @file        tests/unit/core/utf8_test.cpp
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

#include <rex/string/utf8.h>

namespace {

constexpr std::string_view kReplacementUTF8 = "\xEF\xBF\xBD";

std::u16string Replacements(std::size_t count) {
  return std::u16string(count, char16_t{0xFFFD});
}

}  // namespace

REX_TEST_CASE("Well formed text round trips", "[core][utf8]") {
  const std::string text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
  const std::u16string wide = {u'a', char16_t{0x00E9}, char16_t{0x20AC}, char16_t{0xD83D},
                               char16_t{0xDE00}};
  CHECK(rex::ToUTF16(text) == wide);
  CHECK(rex::ToUTF8(wide) == text);
  CHECK(rex::ToUTF8(rex::ToUTF16("")).empty());
}

REX_TEST_CASE("Malformed UTF-8 becomes U+FFFD", "[core][utf8]") {
  CHECK_NOTHROW(
      rex::ToUTF16("a\x80"
                   "b"));
  CHECK(rex::ToUTF16("a\x80"
                     "b") == std::u16string{u'a', char16_t{0xFFFD}, u'b'});
  CHECK(rex::ToUTF16("\xC0\xAF") == Replacements(2));
  CHECK(rex::ToUTF16("\xE0\x80\x80") == Replacements(3));
  CHECK(rex::ToUTF16("\xED\xA0\x80") == Replacements(3));
  CHECK(rex::ToUTF16("\xF4\x90\x80\x80") == Replacements(4));
  CHECK(rex::ToUTF16("\xE2\x82") == Replacements(1));
  CHECK(rex::ToUTF16("\xF5") == Replacements(1));
}

REX_TEST_CASE("Lone surrogates become U+FFFD", "[core][utf8]") {
  CHECK_NOTHROW(rex::ToUTF8(std::u16string{char16_t{0xD800}}));
  CHECK(rex::ToUTF8(std::u16string{char16_t{0xD800}, u'a'}) == std::string(kReplacementUTF8) + "a");
  CHECK(rex::ToUTF8(std::u16string{char16_t{0xDC00}}) == kReplacementUTF8);
  CHECK(rex::ToUTF8(std::u16string{u'a', char16_t{0xD83D}}) == "a" + std::string(kReplacementUTF8));
}

REX_TEST_CASE("Code points are counted, not bytes", "[core][utf8]") {
  CHECK(rex::utf8_count("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80") == 4);
  CHECK(rex::utf8_count("a\x80") == 2);
  CHECK(rex::utf8_count("") == 0);
}

REX_TEST_CASE("Case folding touches only ASCII", "[core][utf8]") {
  CHECK(rex::utf8_lower_ascii("\xC3\x84"
                              "BC\xE2\x82\xAC") ==
        "\xC3\x84"
        "bc\xE2\x82\xAC");
  CHECK(rex::utf8_upper_ascii("\xC3\xA4"
                              "bc") ==
        "\xC3\xA4"
        "BC");
}

REX_TEST_CASE("Path helpers handle multibyte text", "[core][utf8]") {
  CHECK(rex::utf8_find_any_of("\xC3\xA9/x", "/") == 2);
  CHECK(rex::utf8_join_paths("dir\xE2\x82\xAC", "file", U'/') == "dir\xE2\x82\xAC/file");
  CHECK(rex::utf8_join_paths("dir/", "file", U'/') == "dir/file");
  CHECK(rex::utf8_fix_path_separators("\xC3\xA9\\x/y", U'/') == "\xC3\xA9/x/y");
  CHECK(rex::utf8_find_base_path("a/\xE2\x82\xAC/b\xC3\xA9", U'/') == "a/\xE2\x82\xAC");
}

REX_TEST_CASE("utf8_strip_leading_slashes drops only the leading slashes", "[core][utf8]") {
  CHECK(rex::utf8_strip_leading_slashes("//a/b/") == "a/b/");
  CHECK(rex::utf8_strip_leading_slashes("a//b") == "a//b");
  CHECK(rex::utf8_strip_leading_slashes("///").empty());
  CHECK(rex::utf8_strip_leading_slashes("").empty());
}
