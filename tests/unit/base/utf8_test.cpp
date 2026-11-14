/**
 * @file        tests/unit/base/utf8_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <rex/string/text.h>
#include <rex/string/path.h>

namespace {

constexpr std::string_view kReplacementUTF8 = "\xEF\xBF\xBD";

std::u16string Escaped(std::initializer_list<unsigned> bytes) {
  std::u16string result;
  for (const unsigned byte : bytes) {
    result.push_back(static_cast<char16_t>(0xDC00 + byte));
  }
  return result;
}

}  // namespace

REX_TEST_CASE("Well formed text round trips", "[base][utf8]") {
  const std::string text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
  const std::u16string wide = {u'a', char16_t{0x00E9}, char16_t{0x20AC}, char16_t{0xD83D},
                               char16_t{0xDE00}};
  CHECK(rex::string::ToUTF16(text) == wide);
  CHECK(rex::string::ToUTF8(wide) == text);
  CHECK(rex::string::ToUTF8(rex::string::ToUTF16("")).empty());
}

REX_TEST_CASE("Malformed UTF-8 becomes one escaped unit per byte", "[base][utf8]") {
  CHECK_NOTHROW(
      rex::string::ToUTF16("a\x80"
                           "b"));
  CHECK(rex::string::ToUTF16("a\x80"
                             "b") == u"a" + Escaped({0x80}) + u"b");
  CHECK(rex::string::ToUTF16("\xC0\xAF") == Escaped({0xC0, 0xAF}));
  CHECK(rex::string::ToUTF16("\xE0\x80\x80") == Escaped({0xE0, 0x80, 0x80}));
  CHECK(rex::string::ToUTF16("\xED\xA0\x80") == Escaped({0xED, 0xA0, 0x80}));
  CHECK(rex::string::ToUTF16("\xF4\x90\x80\x80") == Escaped({0xF4, 0x90, 0x80, 0x80}));
  CHECK(rex::string::ToUTF16("\xE2\x82") == Escaped({0xE2, 0x82}));
  CHECK(rex::string::ToUTF16("\xF5") == Escaped({0xF5}));
}

REX_TEST_CASE("Lone surrogates become U+FFFD", "[base][utf8]") {
  CHECK_NOTHROW(rex::string::ToUTF8(std::u16string{char16_t{0xD800}}));
  CHECK(rex::string::ToUTF8(std::u16string{char16_t{0xD800}, u'a'}) ==
        std::string(kReplacementUTF8) + "a");
  CHECK(rex::string::ToUTF8(std::u16string{char16_t{0xDC00}}) == kReplacementUTF8);
  CHECK(rex::string::ToUTF8(std::u16string{u'a', char16_t{0xD83D}}) ==
        "a" + std::string(kReplacementUTF8));
}

REX_TEST_CASE("Code points are counted, not bytes", "[base][utf8]") {
  CHECK(rex::string::utf8_count("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80") == 4);
  CHECK(rex::string::utf8_count("a\x80") == 2);
  CHECK(rex::string::utf8_count("") == 0);
}

REX_TEST_CASE("Case folding touches only ASCII", "[base][utf8]") {
  CHECK(rex::string::utf8_lower_ascii("\xC3\x84"
                                      "BC\xE2\x82\xAC") ==
        "\xC3\x84"
        "bc\xE2\x82\xAC");
  CHECK(rex::string::utf8_upper_ascii("\xC3\xA4"
                                      "bc") ==
        "\xC3\xA4"
        "BC");
}

REX_TEST_CASE("Path helpers handle multibyte text", "[base][utf8]") {
  CHECK(rex::string::utf8_find_any_of("\xC3\xA9/x", "/") == 2);
  CHECK(rex::string::utf8_join_paths("dir\xE2\x82\xAC", "file", U'/') == "dir\xE2\x82\xAC/file");
  CHECK(rex::string::utf8_join_paths("dir/", "file", U'/') == "dir/file");
  CHECK(rex::string::utf8_fix_path_separators("\xC3\xA9\\x/y", U'/') == "\xC3\xA9/x/y");
  CHECK(rex::string::utf8_find_base_path("a/\xE2\x82\xAC/b\xC3\xA9", U'/') == "a/\xE2\x82\xAC");
}

REX_TEST_CASE("rex::string::utf8_strip_leading_slashes drops only the leading slashes",
              "[base][utf8]") {
  CHECK(rex::string::utf8_strip_leading_slashes("//a/b/") == "a/b/");
  CHECK(rex::string::utf8_strip_leading_slashes("a//b") == "a//b");
  CHECK(rex::string::utf8_strip_leading_slashes("///").empty());
  CHECK(rex::string::utf8_strip_leading_slashes("").empty());
}

REX_TEST_CASE("Bytes that start no valid sequence keep their value through the case helpers",
              "[base][utf8][invalid]") {
  const std::string shift_jis = "\x82\xA0\x83\x41 DIR\\FILE.BIN";
  CHECK(rex::string::utf8_lower_ascii(shift_jis) == "\x82\xA0\x83\x61 dir\\file.bin");
  CHECK(rex::string::utf8_upper_ascii("\x82\xA0\x83\x61 dir") == "\x82\xA0\x83\x41 DIR");
  CHECK(rex::string::utf8_lower_ascii("\xE2\x82") == "\xE2\x82");
  CHECK(rex::string::utf8_lower_ascii("\xF4\x90\x80\x80") == "\xF4\x90\x80\x80");
  CHECK(rex::string::utf8_lower_ascii("\xC3\x84X\xE2\x82\xAC") == "\xC3\x84x\xE2\x82\xAC");
}

REX_TEST_CASE("Different invalid bytes of the same length are not equal", "[base][utf8][invalid]") {
  CHECK_FALSE(rex::string::utf8_equal_case("\x82\xA0.bin", "\x82\xA2.bin"));
  CHECK_FALSE(rex::string::utf8_equal_case("\xE2\x82", "\xE2\x83"));
  CHECK_FALSE(rex::string::utf8_equal_case_z("\x82\xA0", "\x82\xA2"));
  CHECK_FALSE(rex::string::utf8_equal_z("\x82\xA0", "\x82\xA2"));
  CHECK(rex::string::utf8_equal_case("\x82\xA0.BIN", "\x82\xA0.bin"));
  CHECK(rex::string::utf8_hash_fnv1a_case("\x82\xA0.BIN") ==
        rex::string::utf8_hash_fnv1a_case("\x82\xA0.bin"));
  CHECK(rex::string::utf8_hash_fnv1a_case("\x82\xA0") !=
        rex::string::utf8_hash_fnv1a_case("\x82\xA2"));
}

REX_TEST_CASE("A mixed ASCII and Shift-JIS path starts with its own lowercased ASCII prefix",
              "[base][utf8][invalid]") {
  const std::string path =
      "\x82\xA0"
      "DIR\\\x83\x41.BIN";
  const std::string prefix = rex::string::utf8_lower_ascii(
      "\x82\xA0"
      "DIR\\");
  CHECK(prefix ==
        "\x82\xA0"
        "dir\\");
  CHECK(rex::string::utf8_starts_with_case(path, prefix));
  CHECK_FALSE(rex::string::utf8_starts_with_case(path,
                                                 "\x82\xA2"
                                                 "dir\\"));
  CHECK(rex::string::utf8_ends_with_case(path, "\x83\x41.bin"));
  CHECK_FALSE(rex::string::utf8_ends_with_case(path, "\x83\x42.bin"));
  CHECK(rex::string::utf8_find_first_of_case(path, "\x83\x41") == 6);
}

REX_TEST_CASE("Each invalid byte is its own code point", "[base][utf8][invalid]") {
  CHECK(rex::string::utf8_count("\xE2\x82") == 2);
  CHECK(rex::string::utf8_count("\x82\xA0\x83\x41") == 4);
  CHECK(rex::string::utf8_count("\xE3\x81\x82\x80") == 2);
  CHECK(rex::string::utf8_find_any_of("\x82\xA0/x", "/") == 2);
  CHECK(rex::string::utf8_find_any_of("\x80/x", "\x80") == 0);
  CHECK(rex::string::utf8_find_any_of("\x81/x", "\x80") == std::string_view::npos);
  CHECK(rex::string::utf8_split("\x82\xA0/\x83\x41", "/", true) ==
        std::vector<std::string_view>{"\x82\xA0", "\x83\x41"});
}

REX_TEST_CASE("Path helpers keep invalid bytes", "[base][utf8][invalid]") {
  CHECK(rex::string::utf8_fix_path_separators("\x82\xA0\\\x83\x41", U'/') == "\x82\xA0/\x83\x41");
  CHECK(rex::string::utf8_join_paths("\x82\xA0", "\x83\x41", U'/') == "\x82\xA0/\x83\x41");
  CHECK(rex::string::utf8_canonicalize_path("/\x82\xA0/./x/../\x83\x41", U'/') ==
        "/\x82\xA0/\x83\x41");
  CHECK(rex::string::utf8_find_name_from_path("a/\x82\xA0", U'/') == "\x82\xA0");
}

REX_TEST_CASE("A name with invalid bytes survives UTF-16 byte for byte", "[base][utf8][invalid]") {
  const std::string first = "\x82\xA0\x83\x41.bin";
  const std::string second = "\x82\xA2\x83\x41.bin";
  CHECK(rex::string::ToUTF8(rex::string::ToUTF16(first)) == first);
  CHECK(rex::string::ToUTF8(rex::string::ToUTF16(second)) == second);
  CHECK(rex::string::ToUTF16(first) != rex::string::ToUTF16(second));
  CHECK(rex::string::ToUTF16("\x82\xA0") == Escaped({0x82, 0xA0}));
  const std::string mixed = "\xE3\x81\x82\xE2\x82\xF0\x9F\x98\x80\x80";
  CHECK(rex::string::ToUTF8(rex::string::ToUTF16(mixed)) == mixed);
}

REX_TEST_CASE("Only escaped units map back to bytes from UTF-16", "[base][utf8][invalid]") {
  CHECK(rex::string::ToUTF8(Escaped({0x80, 0xFF})) == "\x80\xFF");
  CHECK(rex::string::ToUTF8(std::u16string{char16_t{0xDC7F}}) == kReplacementUTF8);
  CHECK(rex::string::ToUTF8(std::u16string{char16_t{0xDD00}}) == kReplacementUTF8);
  CHECK(rex::string::ToUTF8(std::u16string{char16_t{0xD800}}) == kReplacementUTF8);
  CHECK(rex::string::ToUTF8(std::u16string{char16_t{0xD83D}, char16_t{0xDE80}}) ==
        "\xF0\x9F\x9A\x80");
}

REX_TEST_CASE("Valid text is unchanged by the UTF-16 round trip", "[base][utf8][invalid]") {
  const std::string text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
  CHECK(rex::string::ToUTF8(rex::string::ToUTF16(text)) == text);
}

REX_TEST_CASE("utf8_upper_ascii uppercases ASCII letters", "[base][string]") {
  REQUIRE(rex::string::utf8_upper_ascii("abc XYZ 1") == "ABC XYZ 1");
}

REX_TEST_CASE("utf8_ends_with_case ignores ASCII case", "[base][string]") {
  REQUIRE(rex::string::utf8_ends_with_case("Default.XEX", ".xex"));
  REQUIRE(rex::string::utf8_ends_with_case("a", "A"));
  REQUIRE_FALSE(rex::string::utf8_ends_with_case("default.xex", ".xexp"));
  REQUIRE_FALSE(rex::string::utf8_ends_with_case("x", "xx"));
  REQUIRE(rex::string::utf8_ends_with_case("abc", ""));
}

REX_TEST_CASE("Wide strings convert to and from UTF-8", "[base][utf8][wide]") {
  const std::string text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
  const std::wstring wide = rex::string::ToWideString(text);
  CHECK(wide.size() == (sizeof(wchar_t) == 2 ? 5u : 4u));
  CHECK(wide.front() == L'a');
  CHECK(wide[1] == wchar_t{0x00E9});
  CHECK(rex::string::ToUTF8(wide) == text);
  CHECK(rex::string::ToWideString("").empty());
  CHECK(rex::string::ToUTF8(std::wstring_view{}).empty());
}

REX_TEST_CASE("Wide conversion replaces invalid input", "[base][utf8][wide]") {
  const std::wstring wide =
      rex::string::ToWideString(std::string_view("a\xFF"
                                                 "b",
                                                 3));
  REQUIRE(wide.size() == 3);
  CHECK(wide[0] == L'a');
  CHECK(wide[1] == wchar_t{0xFFFD});
  CHECK(wide[2] == L'b');
  const std::wstring lone_surrogate = {L'a', wchar_t{0xD800}, L'b'};
  CHECK(rex::string::ToUTF8(lone_surrogate) ==
        std::string("a") + std::string(kReplacementUTF8) + "b");
}

REX_TEST_CASE("Shift-JIS converts ASCII and Japanese text", "[base][utf8][shiftjis]") {
  CHECK(rex::string::ShiftJISToUTF8("Hello 123") == "Hello 123");
  CHECK(rex::string::UTF8ToShiftJIS("Hello 123") == "Hello 123");
  const std::string sjis = "\x93\xFA\x96\x7B\x8C\xEA";
  const std::string utf8 = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";
  CHECK(rex::string::ShiftJISToUTF8(sjis) == utf8);
  CHECK(rex::string::UTF8ToShiftJIS(utf8) == sjis);
  const std::string katakana_sjis = "\x83\x65\x83\x58\x83\x67";
  const std::string katakana_utf8 = "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88";
  CHECK(rex::string::ShiftJISToUTF8(katakana_sjis) == katakana_utf8);
  CHECK(rex::string::UTF8ToShiftJIS(katakana_utf8) == katakana_sjis);
}

REX_TEST_CASE("Shift-JIS uses the code page 932 mappings", "[base][utf8][shiftjis]") {
  const std::string circled_one_sjis = "\x87\x40";
  const std::string circled_one_utf8 = "\xE2\x91\xA0";
  CHECK(rex::string::ShiftJISToUTF8(circled_one_sjis) == circled_one_utf8);
  CHECK(rex::string::UTF8ToShiftJIS(circled_one_utf8) == circled_one_sjis);
  const std::string wave_dash_sjis = "\x81\x60";
  const std::string fullwidth_tilde_utf8 = "\xEF\xBD\x9E";
  CHECK(rex::string::ShiftJISToUTF8(wave_dash_sjis) == fullwidth_tilde_utf8);
  CHECK(rex::string::UTF8ToShiftJIS(fullwidth_tilde_utf8) == wave_dash_sjis);
}

REX_TEST_CASE("Shift-JIS conversion survives invalid input", "[base][utf8][shiftjis]") {
  const std::string decoded =
      rex::string::ShiftJISToUTF8(std::string_view("ab\xFF"
                                                   "cd",
                                                   5));
  CHECK(decoded.starts_with("ab"));
  CHECK(decoded.ends_with("cd"));
  const std::string encoded =
      rex::string::UTF8ToShiftJIS(std::string_view("ab\xFF"
                                                   "cd",
                                                   5));
  CHECK(encoded.starts_with("ab"));
  CHECK(encoded.ends_with("cd"));
  const std::string unmappable = rex::string::UTF8ToShiftJIS(
      "a\xF0\x9F\x98\x80"
      "b");
  CHECK(unmappable.starts_with("a?"));
  CHECK(unmappable.ends_with("b"));
}

REX_TEST_CASE("Shift-JIS conversion of empty input is empty", "[base][utf8][shiftjis]") {
  CHECK(rex::string::ShiftJISToUTF8("").empty());
  CHECK(rex::string::UTF8ToShiftJIS("").empty());
}
