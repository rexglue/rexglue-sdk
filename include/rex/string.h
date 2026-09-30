/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#include <string.h>

#include <rex/memory/utils.h>
#include <rex/platform.h>
#include <rex/string/utf8.h>

#if !REX_PLATFORM_WIN32
#include <strings.h>
#endif

namespace rex {

inline int compare_case(const char* string1, const char* string2) {
#if REX_PLATFORM_WIN32
  return _stricmp(string1, string2);
#else
  return strcasecmp(string1, string2);
#endif
}

inline int compare_case_n(const char* string1, const char* string2, size_t count) {
#if REX_PLATFORM_WIN32
  return _strnicmp(string1, string2, count);
#else
  return strncasecmp(string1, string2, count);
#endif
}

inline constexpr std::string_view kWhitespace = " \t\r\n";

inline std::string_view trim_left(std::string_view sv, std::string_view chars = kWhitespace) {
  auto start = sv.find_first_not_of(chars);
  return start == std::string_view::npos ? std::string_view{} : sv.substr(start);
}

inline std::string_view trim_right(std::string_view sv, std::string_view chars = kWhitespace) {
  auto end = sv.find_last_not_of(chars);
  return end == std::string_view::npos ? std::string_view{} : sv.substr(0, end + 1);
}

inline std::string_view trim(std::string_view sv, std::string_view chars = kWhitespace) {
  return trim_right(trim_left(sv, chars), chars);
}

inline std::string trim_string(std::string_view sv, std::string_view chars = kWhitespace) {
  return std::string(trim(sv, chars));
}

// Caller must free() the returned pointer.
inline char* duplicate(const char* source) {
#if REX_PLATFORM_WIN32
  return _strdup(source);
#else
  return strdup(source);
#endif
}

inline size_t copy_truncating(char* dest, const std::string_view source, size_t dest_buffer_count) {
  if (!dest_buffer_count) {
    return 0;
  }
  size_t chars_copied = std::min(source.size(), dest_buffer_count - size_t(1));
  std::memcpy(dest, source.data(), chars_copied);
  dest[chars_copied] = '\0';
  return chars_copied;
}

inline size_t copy_truncating(char16_t* dest, const std::u16string_view source,
                              size_t dest_buffer_count) {
  if (!dest_buffer_count) {
    return 0;
  }
  size_t chars_copied = std::min(source.size(), dest_buffer_count - size_t(1));
  std::memcpy(dest, source.data(), chars_copied * sizeof(char16_t));
  dest[chars_copied] = u'\0';
  return chars_copied;
}

inline size_t copy_and_swap_truncating(char16_t* dest, const std::u16string_view source,
                                       size_t dest_buffer_count) {
  if (!dest_buffer_count) {
    return 0;
  }
  size_t chars_copied = std::min(source.size(), dest_buffer_count - size_t(1));
  memory::copy_and_swap(dest, source.data(), chars_copied);
  dest[chars_copied] = u'\0';
  return chars_copied;
}

// No in-buffer NUL terminator. Caller must zero the buffer (or rely on
// adjacent padding bytes) to terminate.
inline size_t copy_unterminated(char* dest, const std::string_view source,
                                size_t dest_buffer_count) {
  if (!dest_buffer_count) {
    return 0;
  }
  size_t chars_copied = std::min(source.size(), dest_buffer_count);
  std::memcpy(dest, source.data(), chars_copied);
  return chars_copied;
}

}  // namespace rex
