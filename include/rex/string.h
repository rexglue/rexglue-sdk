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

#pragma once

#include <cstring>
#include <string>
#include <string_view>

#include <rex/string/utf8.h>

namespace rex::string {

// Basic string comparison (case-insensitive)
int compare_case(const char* string1, const char* string2);
int compare_case_n(const char* string1, const char* string2, size_t count);

// Whitespace trimming
inline constexpr std::string_view kWhitespace = " \t\r\n";

/// Trim whitespace from the left of a string_view
std::string_view trim_left(std::string_view sv, std::string_view chars = kWhitespace);

/// Trim whitespace from the right of a string_view
std::string_view trim_right(std::string_view sv, std::string_view chars = kWhitespace);

/// Trim whitespace from both ends of a string_view
std::string_view trim(std::string_view sv, std::string_view chars = kWhitespace);

/// Trim and convert to string
std::string trim_string(std::string_view sv, std::string_view chars = kWhitespace);

// String duplication
char* duplicate(const char* source);

// Encoding conversion
std::string to_utf8(const std::u16string_view source);
std::u16string to_utf16(const std::string_view source);

// Safe string copy - copies up to max_count chars and null-terminates
void rex_strcpy(char* dest, size_t dest_size, const char* src, size_t max_count = 0);

}  // namespace rex::string
