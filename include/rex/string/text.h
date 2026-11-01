/**
 * @file        string/text.h
 * @brief       ASCII and UTF-8 text operations, the string builder and the string keys
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/string.h, xenia/base/utf8.h,
 *              xenia/base/string_buffer.h and xenia/base/string_key.h (Ben Vanik, 2020).
 */

#pragma once

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>
#include <string.h>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <rex/memory/endian.h>
#include <rex/platform.h>
#include <rex/types.h>

#if !REX_PLATFORM_WIN32
#include <strings.h>
#endif

namespace rex::string {

/**
 * Compares two NUL terminated strings, ignoring ASCII case.
 *
 * @return Negative, 0 or positive as string1 orders before, equal to or after string2.
 */
inline i32 CompareIgnoreCase(const char* string1, const char* string2) {
#if REX_PLATFORM_WIN32
  return _stricmp(string1, string2);
#else
  return strcasecmp(string1, string2);
#endif
}

/**
 * Compares up to count characters of two strings, ignoring ASCII case.
 *
 * @param count Maximum number of characters to compare.
 * @return Negative, 0 or positive as string1 orders before, equal to or after string2.
 */
inline i32 CompareIgnoreCaseN(const char* string1, const char* string2, size_t count) {
#if REX_PLATFORM_WIN32
  return _strnicmp(string1, string2, count);
#else
  return strncasecmp(string1, string2, count);
#endif
}

namespace detail {

template <typename CharT>
inline constexpr CharT kWhitespaceChars[] = {static_cast<CharT>(' '), static_cast<CharT>('\t'),
                                             static_cast<CharT>('\r'), static_cast<CharT>('\n')};

}  // namespace detail

/** The characters the trim functions remove by default, as a view of the character type. */
template <typename CharT>
inline constexpr std::basic_string_view<CharT> kWhitespaceOf{detail::kWhitespaceChars<CharT>, 4};

/** Characters the trim functions remove by default. */
inline constexpr std::string_view kWhitespace = kWhitespaceOf<char>;

/**
 * Removes leading characters.
 *
 * @return View of sv without the leading characters, empty when every character matches.
 */
template <typename CharT>
inline std::basic_string_view<CharT> TrimLeft(
    std::basic_string_view<CharT> sv, std::basic_string_view<CharT> chars = kWhitespaceOf<CharT>) {
  auto start = sv.find_first_not_of(chars);
  return start == std::basic_string_view<CharT>::npos ? std::basic_string_view<CharT>{}
                                                      : sv.substr(start);
}

/**
 * Removes trailing characters.
 *
 * @return View of sv without the trailing characters, empty when every character matches.
 */
template <typename CharT>
inline std::basic_string_view<CharT> TrimRight(
    std::basic_string_view<CharT> sv, std::basic_string_view<CharT> chars = kWhitespaceOf<CharT>) {
  auto end = sv.find_last_not_of(chars);
  return end == std::basic_string_view<CharT>::npos ? std::basic_string_view<CharT>{}
                                                    : sv.substr(0, end + 1);
}

/**
 * Removes leading and trailing characters.
 *
 * @return View of sv without the leading and trailing characters.
 */
template <typename CharT>
inline std::basic_string_view<CharT> Trim(
    std::basic_string_view<CharT> sv, std::basic_string_view<CharT> chars = kWhitespaceOf<CharT>) {
  return TrimRight(TrimLeft(sv, chars), chars);
}

/**
 * Removes leading characters.
 *
 * @return View of sv without the leading characters, empty when every character matches.
 */
inline std::string_view TrimLeft(std::string_view sv, std::string_view chars = kWhitespace) {
  return TrimLeft<char>(sv, chars);
}

/**
 * Removes trailing characters.
 *
 * @return View of sv without the trailing characters, empty when every character matches.
 */
inline std::string_view TrimRight(std::string_view sv, std::string_view chars = kWhitespace) {
  return TrimRight<char>(sv, chars);
}

/**
 * Removes leading and trailing characters.
 *
 * @return View of sv without the leading and trailing characters.
 */
inline std::string_view Trim(std::string_view sv, std::string_view chars = kWhitespace) {
  return Trim<char>(sv, chars);
}

/**
 * Removes leading and trailing characters into a new string.
 *
 * @return Copy of sv without the leading and trailing characters.
 */
inline std::string TrimString(std::string_view sv, std::string_view chars = kWhitespace) {
  return std::string(Trim(sv, chars));
}

/**
 * Copies a NUL terminated string to the heap.
 *
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
 * @param dest_buffer_count Size of dest in characters, including the terminator.
 * @return Characters copied, not counting the terminator.
 */
inline size_t CopyTruncating(char* dest, const std::string_view source, size_t dest_buffer_count) {
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
 * @param dest_buffer_count Size of dest in code units, including the terminator.
 * @return Code units copied, not counting the terminator.
 */
inline size_t CopyTruncating(char16_t* dest, const std::u16string_view source,
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
 * Copies a UTF-16 string into a fixed buffer, byte swapping each code unit.
 *
 * @param dest_buffer_count Size of dest in code units, including the terminator.
 * @return Code units copied, not counting the terminator.
 */
inline size_t CopyAndSwapTruncating(char16_t* dest, const std::u16string_view source,
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
 * Copies a string into a fixed buffer, truncating to fit, without a terminator.
 *
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

/**
 * Lowercases the ASCII letters of a string. Other code points pass through.
 *
 * @param view UTF-8 text.
 */
REX_API std::string utf8_lower_ascii(const std::string_view view);

/**
 * Uppercases the ASCII letters of a string.
 *
 * @param view UTF-8 text.
 */
REX_API std::string utf8_upper_ascii(const std::string_view view);

/**
 * Hashes the code points of a string with FNV-1a.
 *
 * @param view UTF-8 text.
 */
REX_API size_t utf8_hash_fnv1a(const std::string_view view);

/**
 * Hashes the code points of a string with FNV-1a, with ASCII letters lowercased first.
 *
 * @param view UTF-8 text.
 */
REX_API size_t utf8_hash_fnv1a_case(const std::string_view view);

/**
 * Splits a string on any of a set of delimiter code points.
 *
 * @param needles Delimiter code points.
 * @param remove_empty True to omit empty parts between adjacent delimiters.
 * @return Views of the parts, in order.
 */
REX_API std::vector<std::string_view> utf8_split(const std::string_view haystack,
                                                 const std::string_view needles,
                                                 bool remove_empty = false);

/**
 * Reports whether two strings match up to the first NUL in each.
 *
 * @return True when the strings match up to their terminators.
 */
REX_API bool utf8_equal_z(const std::string_view left, const std::string_view right);

REX_API bool utf8_equal_case(const std::string_view left, const std::string_view right);

/**
 * Reports whether two strings match up to the first NUL in each, ignoring ASCII case.
 *
 * @return True when the strings match up to their terminators.
 */
REX_API bool utf8_equal_case_z(const std::string_view left, const std::string_view right);

/**
 * Finds the first code point that matches any of a set.
 *
 * @return Byte offset of the match, 0 when needles is empty, or npos when nothing matches.
 */
REX_API std::string_view::size_type utf8_find_any_of(const std::string_view haystack,
                                                     const std::string_view needles);

/**
 * Finds the first code point that matches any of a set, ignoring ASCII case.
 *
 * @return Byte offset of the match, 0 when needles is empty, or npos when nothing matches.
 */
REX_API std::string_view::size_type utf8_find_any_of_case(const std::string_view haystack,
                                                          const std::string_view needles);

/**
 * Finds the first occurrence of a substring.
 *
 * @return Byte offset of the match, 0 when needle is empty, or npos when nothing matches.
 */
REX_API std::string_view::size_type utf8_find_first_of(const std::string_view haystack,
                                                       const std::string_view needle);

/**
 * Finds the first occurrence of a substring, ignoring ASCII case.
 *
 * @return Byte offset of the match, 0 when needle is empty, or npos when nothing matches.
 */
REX_API std::string_view::size_type utf8_find_first_of_case(const std::string_view haystack,
                                                            const std::string_view needle);

/**
 * Reports whether a string starts with a prefix.
 *
 * @return True when haystack starts with needle or needle is empty.
 */
REX_API bool utf8_starts_with(const std::string_view haystack, const std::string_view needle);

/**
 * Reports whether a string starts with a prefix, ignoring ASCII case.
 *
 * @return True when haystack starts with needle or needle is empty.
 */
REX_API bool utf8_starts_with_case(const std::string_view haystack, const std::string_view needle);

/**
 * Reports whether a string ends with a suffix.
 *
 * @return True when haystack ends with needle or needle is empty.
 */
REX_API bool utf8_ends_with(const std::string_view haystack, const std::string_view needle);

/**
 * Reports whether a string ends with a suffix, ignoring ASCII case.
 *
 * @return True when haystack ends with needle or needle is empty.
 */
REX_API bool utf8_ends_with_case(const std::string_view haystack, const std::string_view needle);

/**
 * Converts UTF-16 to UTF-8.
 *
 * @param source UTF-16 text.
 * @return The UTF-8 text.
 */
REX_API std::string ToUTF8(const std::u16string_view source);

/**
 * Converts UTF-8 to UTF-16.
 *
 * @param source UTF-8 text.
 * @return The UTF-16 text.
 */
REX_API std::u16string ToUTF16(const std::string_view source);

/**
 * Converts UTF-8 to the host wide string, UTF-16 or UTF-32 by wchar_t size. Invalid UTF-8 becomes
 * U+FFFD.
 *
 * @param source UTF-8 text.
 */
REX_API std::wstring ToWideString(const std::string_view source);

/**
 * Converts the host wide string, UTF-16 or UTF-32 by wchar_t size, to UTF-8. Invalid code units
 * become U+FFFD.
 *
 * @return The UTF-8 text.
 */
REX_API std::string ToUTF8(const std::wstring_view source);

/**
 * Converts Shift-JIS (code page 932) to UTF-8. Returns the input unchanged without the platform
 * converter.
 *
 * @param source Shift-JIS text.
 * @return The UTF-8 text.
 */
REX_API std::string ShiftJISToUTF8(const std::string_view source);

/**
 * Converts UTF-8 to Shift-JIS (code page 932). Unrepresentable characters become '?'. Returns the
 * input unchanged without the platform converter.
 *
 * @param source UTF-8 text.
 * @return The Shift-JIS text.
 */
REX_API std::string UTF8ToShiftJIS(const std::string_view source);

/**
 * Converts a host path to UTF-8.
 *
 * @return The path as UTF-8 text.
 */
REX_API std::string PathToUTF8(const std::filesystem::path& path);

/**
 * Converts a host path to UTF-16.
 *
 * @return The path as UTF-16 text.
 */
REX_API std::u16string PathToUTF16(const std::filesystem::path& path);

/**
 * Builds a host path from UTF-8 text.
 *
 * @param source UTF-8 path text.
 */
REX_API std::filesystem::path to_path(const std::string_view source);

/**
 * Builds a host path from UTF-16 text.
 *
 * @param source UTF-16 path text.
 */
REX_API std::filesystem::path to_path(const std::u16string_view source);

/**
 * Counts code points.
 *
 * @param view UTF-8 text.
 * @return Number of code points.
 */
REX_API size_t utf8_count(const std::string_view view);

/** Growable character buffer that stays NUL terminated after every append. */
class REX_API StringBuffer {
 public:
  /**
   * Allocates the buffer.
   *
   * @param initial_capacity Initial capacity in bytes. The buffer holds at least 16 KiB.
   */
  explicit StringBuffer(size_t initial_capacity = 0);
  ~StringBuffer();

  /** Returns the characters. The contents are NUL terminated. */
  char* GetBuffer() const { return buffer_; }

  /** Returns the content length. The count excludes the terminator. */
  size_t GetLength() const { return length_; }

  /** Empties the buffer and keeps its capacity. */
  void Reset();

  void Append(char c);

  /** Appends a character count times. */
  void Append(char c, size_t count);

  void Append(const char* value);

  void Append(const std::string_view value);

  /**
   * Appends text formatted with std::format.
   *
   * @param format Format string, checked at compile time.
   */
  template <typename... Args>
  void AppendFormat(std::format_string<const Args&...> format, const Args&... args) {
    Append(std::format(format, args...));
  }

  /**
   * Appends text formatted with vsnprintf.
   *
   * @param format printf format string.
   */
  void AppendVarargs(const char* format, va_list args);

  void AppendBytes(const u8* buffer, size_t length);

  std::string ToString();

  /** Returns a view of the contents. The view is valid until the next append or destruction. */
  std::string_view ToStringView() const;

  /**
   * Copies the contents into a byte vector.
   *
   * @return The contents without the terminator.
   */
  std::vector<u8> ToBytes() const;

 private:
  void Grow(size_t additional_length);

  char* buffer_ = nullptr;
  size_t length_ = 0;
  size_t buffer_capacity_ = 0;
};

namespace detail {

struct StringKeyBase {
 private:
  std::variant<std::string, std::string_view> value_;

 public:
  explicit StringKeyBase(const std::string_view value) : value_(value) {}
  explicit StringKeyBase(std::string value) : value_(std::move(value)) {}

  std::string_view GetView() const {
    if (const auto* owned = std::get_if<std::string>(&value_)) {
      return *owned;
    }
    const auto* borrowed = std::get_if<std::string_view>(&value_);
    return borrowed ? *borrowed : std::string_view();
  }
};

}  // namespace detail

/** Case sensitive string key that borrows a view or owns a copy. */
struct StringKey : detail::StringKeyBase {
 public:
  /** Borrows a string. The string outlives the key. */
  explicit StringKey(const std::string_view value) : StringKeyBase(value) {}

  /** Owns a string. */
  explicit StringKey(std::string value) : StringKeyBase(std::move(value)) {}

  /** Creates a key that owns a copy of a string. */
  static StringKey Create(const std::string_view value) { return StringKey(std::string(value)); }

  /** Creates a key that owns a string. */
  static StringKey Create(std::string value) { return StringKey(std::move(value)); }

  /** Compares two keys byte for byte. */
  bool operator==(const StringKey& other) const { return other.GetView() == GetView(); }

  /**
   * @return FNV-1a hash of the code points.
   */
  size_t hash() const { return rex::string::utf8_hash_fnv1a(GetView()); }

  struct Hash {
    size_t operator()(const StringKey& t) const { return t.hash(); }
  };
};

/** String key that ignores ASCII case, borrowing a view or owning a copy. */
struct StringKeyIgnoreCase : detail::StringKeyBase {
 public:
  /** Borrows a string. The string outlives the key. */
  explicit StringKeyIgnoreCase(const std::string_view value) : StringKeyBase(value) {}

  /** Owns a string. */
  explicit StringKeyIgnoreCase(std::string value) : StringKeyBase(std::move(value)) {}

  /** Creates a key that owns a copy of a string. */
  static StringKeyIgnoreCase Create(const std::string_view value) {
    return StringKeyIgnoreCase(std::string(value));
  }

  /** Creates a key that owns a string. */
  static StringKeyIgnoreCase Create(std::string value) {
    return StringKeyIgnoreCase(std::move(value));
  }

  /** Compares two keys, ignoring ASCII case. */
  bool operator==(const StringKeyIgnoreCase& other) const {
    return rex::string::utf8_equal_case(other.GetView(), GetView());
  }

  /**
   * Hashes the key, ignoring ASCII case.
   *
   * @return FNV-1a hash of the code points with ASCII letters lowercased.
   */
  size_t hash() const { return rex::string::utf8_hash_fnv1a_case(GetView()); }

  struct Hash {
    size_t operator()(const StringKeyIgnoreCase& t) const { return t.hash(); }
  };
};

}  // namespace rex::string

namespace std {
template <>
struct hash<rex::string::StringKey> {
  std::size_t operator()(const rex::string::StringKey& t) const { return t.hash(); }
};

template <>
struct hash<rex::string::StringKeyIgnoreCase> {
  std::size_t operator()(const rex::string::StringKeyIgnoreCase& t) const { return t.hash(); }
};
}  // namespace std
