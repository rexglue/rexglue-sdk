/**
 * @file        string.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/string.h (Ben Vanik, 2020).
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#include <string.h>

#include <rex/memory/byte_swap.h>
#include <rex/platform.h>
#include <rex/string/utf8.h>
#include <rex/types.h>

#if !REX_PLATFORM_WIN32
#include <strings.h>
#endif

namespace rex {

/**
 * Compares 2 NUL terminated strings, ignoring ASCII case.
 *
 * @param string1 First string.
 * @param string2 Second string.
 * @return Negative, 0 or positive as string1 orders before, equal to or after string2.
 */
inline i32 CompareCase(const char* string1, const char* string2) {
#if REX_PLATFORM_WIN32
  return _stricmp(string1, string2);
#else
  return strcasecmp(string1, string2);
#endif
}

/**
 * Compares up to count characters of 2 strings, ignoring ASCII case.
 *
 * @param string1 First string.
 * @param string2 Second string.
 * @param count Maximum number of characters to compare.
 * @return Negative, 0 or positive as string1 orders before, equal to or after string2.
 */
inline i32 CompareCaseN(const char* string1, const char* string2, size_t count) {
#if REX_PLATFORM_WIN32
  return _strnicmp(string1, string2, count);
#else
  return strncasecmp(string1, string2, count);
#endif
}

/** Characters the trim functions remove by default. */
inline constexpr std::string_view kWhitespace = " \t\r\n";

/**
 * Removes leading characters.
 *
 * @param sv String to trim.
 * @param chars Characters to remove.
 * @return View of sv without the leading characters, empty when every character matches.
 */
inline std::string_view TrimLeft(std::string_view sv, std::string_view chars = kWhitespace) {
  auto start = sv.find_first_not_of(chars);
  return start == std::string_view::npos ? std::string_view{} : sv.substr(start);
}

/**
 * Removes trailing characters.
 *
 * @param sv String to trim.
 * @param chars Characters to remove.
 * @return View of sv without the trailing characters, empty when every character matches.
 */
inline std::string_view TrimRight(std::string_view sv, std::string_view chars = kWhitespace) {
  auto end = sv.find_last_not_of(chars);
  return end == std::string_view::npos ? std::string_view{} : sv.substr(0, end + 1);
}

/**
 * Removes leading and trailing characters.
 *
 * @param sv String to trim.
 * @param chars Characters to remove.
 * @return View of sv without the leading and trailing characters.
 */
inline std::string_view Trim(std::string_view sv, std::string_view chars = kWhitespace) {
  return TrimRight(TrimLeft(sv, chars), chars);
}

/**
 * Removes leading and trailing characters into a new string.
 *
 * @param sv String to trim.
 * @param chars Characters to remove.
 * @return Copy of sv without the leading and trailing characters.
 */
inline std::string TrimString(std::string_view sv, std::string_view chars = kWhitespace) {
  return std::string(Trim(sv, chars));
}

/**
 * Copies a NUL terminated string to the heap.
 *
 * @param source String to copy.
 * @return The copy. The caller frees it with free.
 */
inline char* Duplicate(const char* source) {
#if REX_PLATFORM_WIN32
  return _strdup(source);
#else
  return strdup(source);
#endif
}

/**
 * Copies a string into a fixed buffer, truncating to fit, and NUL terminates it.
 *
 * @param dest Destination buffer.
 * @param source String to copy.
 * @param dest_buffer_count Size of dest in characters, including the terminator.
 * @return Characters copied, not counting the terminator.
 */
inline size_t copy_truncating(char* dest, const std::string_view source, size_t dest_buffer_count) {
  if (!dest_buffer_count) {
    return 0;
  }
  size_t chars_copied = std::min(source.size(), dest_buffer_count - static_cast<size_t>(1));
  std::memcpy(dest, source.data(), chars_copied);
  dest[chars_copied] = '\0';
  return chars_copied;
}

/**
 * Copies a UTF-16 string into a fixed buffer, truncating to fit, and NUL terminates it.
 *
 * @param dest Destination buffer.
 * @param source String to copy.
 * @param dest_buffer_count Size of dest in code units, including the terminator.
 * @return Code units copied, not counting the terminator.
 */
inline size_t copy_truncating(char16_t* dest, const std::u16string_view source,
                              size_t dest_buffer_count) {
  if (!dest_buffer_count) {
    return 0;
  }
  size_t chars_copied = std::min(source.size(), dest_buffer_count - static_cast<size_t>(1));
  std::memcpy(dest, source.data(), chars_copied * sizeof(char16_t));
  dest[chars_copied] = u'\0';
  return chars_copied;
}

/**
 * Copies a UTF-16 string into a fixed buffer with each code unit byte swapped, truncating to
 * fit, and NUL terminates it.
 *
 * @param dest Destination buffer.
 * @param source String to copy.
 * @param dest_buffer_count Size of dest in code units, including the terminator.
 * @return Code units copied, not counting the terminator.
 */
inline size_t copy_and_swap_truncating(char16_t* dest, const std::u16string_view source,
                                       size_t dest_buffer_count) {
  if (!dest_buffer_count) {
    return 0;
  }
  size_t chars_copied = std::min(source.size(), dest_buffer_count - static_cast<size_t>(1));
  const std::u16string_view head = source.substr(0, chars_copied);
  memory::copy_and_swap(dest, head.data(), head.size());
  dest[chars_copied] = u'\0';
  return chars_copied;
}

/**
 * Copies a string into a fixed buffer, truncating to fit, without a terminator. A source that
 * fills the buffer leaves no NUL, so the caller zeroes the buffer or relies on the bytes after
 * it.
 *
 * @param dest Destination buffer.
 * @param source String to copy.
 * @param dest_buffer_count Size of dest in characters.
 * @return Characters copied.
 */
inline size_t CopyUnterminated(char* dest, const std::string_view source,
                               size_t dest_buffer_count) {
  if (!dest_buffer_count) {
    return 0;
  }
  size_t chars_copied = std::min(source.size(), dest_buffer_count);
  std::memcpy(dest, source.data(), chars_copied);
  return chars_copied;
}

}  // namespace rex
