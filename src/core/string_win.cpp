/**
 * @file        core/string_win.cpp
 * @brief       Windows platform string function implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>
#include <rex/string.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include <string.h>

namespace rex::string {

int compare_case(const char* string1, const char* string2) {
  return _stricmp(string1, string2);
}

int compare_case_n(const char* string1, const char* string2, size_t count) {
  return _strnicmp(string1, string2, count);
}

char* duplicate(const char* source) {
  return _strdup(source);
}

void rex_strcpy(char* dest, size_t dest_size, const char* src, size_t max_count) {
  if (dest_size == 0)
    return;
  size_t count = (max_count == 0) ? _TRUNCATE : max_count;
  strncpy_s(dest, dest_size, src, count);
}

}  // namespace rex::string
