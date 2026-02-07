/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 Tom Clay <tomc@tctechstuff.com>
 *              Modified for rexglue - Xbox 360 recompilation framework
 *
 * @changes     - Moved functions from rex:: to rex::string:: namespace
 *              - Renamed xe_* functions to cleaner names
 */

#include <rex/string.h>

#include <string.h>
#include <algorithm>
#include <locale>

#include <rex/platform.h>

#if !REX_PLATFORM_WIN32
#include <strings.h>
#endif  // !REX_PLATFORM_WIN32

#define UTF_CPP_CPLUSPLUS 201703L
#include <utf8.h>

namespace utfcpp = utf8;

namespace rex::string {

int compare_case(const char* string1, const char* string2) {
#if REX_PLATFORM_WIN32
  return _stricmp(string1, string2);
#else
  return strcasecmp(string1, string2);
#endif  // REX_PLATFORM_WIN32
}

int compare_case_n(const char* string1, const char* string2, size_t count) {
#if REX_PLATFORM_WIN32
  return _strnicmp(string1, string2, count);
#else
  return strncasecmp(string1, string2, count);
#endif  // REX_PLATFORM_WIN32
}

char* duplicate(const char* source) {
#if REX_PLATFORM_WIN32
  return _strdup(source);
#else
  return strdup(source);
#endif  // REX_PLATFORM_WIN32
}

std::string to_utf8(const std::u16string_view source) {
  return utfcpp::utf16to8(source);
}

std::u16string to_utf16(const std::string_view source) {
  return utfcpp::utf8to16(source);
}

std::string_view trim_left(std::string_view sv, std::string_view chars) {
  auto start = sv.find_first_not_of(chars);
  return start == std::string_view::npos ? std::string_view{} : sv.substr(start);
}

std::string_view trim_right(std::string_view sv, std::string_view chars) {
  auto end = sv.find_last_not_of(chars);
  return end == std::string_view::npos ? std::string_view{} : sv.substr(0, end + 1);
}

std::string_view trim(std::string_view sv, std::string_view chars) {
  return trim_right(trim_left(sv, chars), chars);
}

std::string trim_string(std::string_view sv, std::string_view chars) {
  return std::string(trim(sv, chars));
}

}  // namespace rex::string
