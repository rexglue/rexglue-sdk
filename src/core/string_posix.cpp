/**
 * @file        core/string_posix.cpp
 * @brief       POSIX platform string function implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>
#include <rex/string.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <string.h>

#include <algorithm>

#include <strings.h>

namespace rex::string {

int compare_case(const char* string1, const char* string2) {
  return strcasecmp(string1, string2);
}

int compare_case_n(const char* string1, const char* string2, size_t count) {
  return strncasecmp(string1, string2, count);
}

char* duplicate(const char* source) {
  return strdup(source);
}

void rex_strcpy(char* dest, size_t dest_size, const char* src, size_t max_count) {
  if (dest_size == 0)
    return;
  size_t copy_size = (max_count == 0) ? dest_size - 1 : std::min(max_count, dest_size - 1);
  strncpy(dest, src, copy_size);
  dest[copy_size] = '\0';
}

}  // namespace rex::string
