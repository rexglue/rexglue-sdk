/**
 * @file        string/utf8.h
 * @brief       UTF-8 conversion, comparison, search and path helpers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/utf8.h (Ben Vanik, 2020).
 */

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <rex/macros.h>
#include <rex/platform.h>

namespace rex {

/** Host path separator. */
constexpr char32_t kUTF8PathSeparator = static_cast<char32_t>(rex::platform::kPathSeparator);
/** Guest path separator. */
constexpr char32_t kUTF8GuestPathSeparator = U'\\';

/**
 * Converts UTF-16 to UTF-8.
 *
 * @param source UTF-16 text.
 * @return The UTF-8 text.
 */
REX_API std::string to_utf8(const std::u16string_view source);

/**
 * Converts UTF-8 to UTF-16.
 *
 * @param source UTF-8 text.
 * @return The UTF-16 text.
 */
REX_API std::u16string to_utf16(const std::string_view source);

/**
 * Converts a host path to UTF-8.
 *
 * @param path Host path.
 * @return The path as UTF-8 text.
 */
REX_API std::string path_to_utf8(const std::filesystem::path& path);

/**
 * Converts a host path to UTF-16.
 *
 * @param path Host path.
 * @return The path as UTF-16 text.
 */
REX_API std::u16string path_to_utf16(const std::filesystem::path& path);

/**
 * Builds a host path from UTF-8 text.
 *
 * @param source UTF-8 path text.
 * @return The host path.
 */
REX_API std::filesystem::path to_path(const std::string_view source);

/**
 * Builds a host path from UTF-16 text.
 *
 * @param source UTF-16 path text.
 * @return The host path.
 */
REX_API std::filesystem::path to_path(const std::u16string_view source);

/**
 * Counts code points.
 *
 * @param view UTF-8 text.
 * @return Number of code points.
 */
REX_API size_t utf8_count(const std::string_view view);

/**
 * Lowercases the ASCII letters of a string. Other code points pass through.
 *
 * @param view UTF-8 text.
 * @return The converted text.
 */
REX_API std::string utf8_lower_ascii(const std::string_view view);

/**
 * Uppercases the ASCII letters of a string.
 *
 * @param view UTF-8 text.
 * @return The converted text.
 */
REX_API std::string utf8_upper_ascii(const std::string_view view);

/**
 * Hashes the code points of a string with FNV-1a.
 *
 * @param view UTF-8 text.
 * @return The hash.
 */
REX_API size_t utf8_hash_fnv1a(const std::string_view view);

/**
 * Hashes the code points of a string with FNV-1a, with ASCII letters lowercased first.
 *
 * @param view UTF-8 text.
 * @return The hash.
 */
REX_API size_t utf8_hash_fnv1a_case(const std::string_view view);

/**
 * Splits a string on any of a set of delimiter code points.
 *
 * @param haystack Text to split.
 * @param needles Delimiter code points.
 * @param remove_empty True to drop empty parts between adjacent delimiters.
 * @return Views of the parts, in order.
 */
REX_API std::vector<std::string_view> utf8_split(const std::string_view haystack,
                                                 const std::string_view needles,
                                                 bool remove_empty = false);

/**
 * Compares 2 strings up to the first NUL in each.
 *
 * @param left First string.
 * @param right Second string.
 * @return True when the strings match up to their terminators.
 */
REX_API bool utf8_equal_z(const std::string_view left, const std::string_view right);

/**
 * Compares 2 strings, ignoring ASCII case.
 *
 * @param left First string.
 * @param right Second string.
 * @return True when the strings match.
 */
REX_API bool utf8_equal_case(const std::string_view left, const std::string_view right);

/**
 * Compares 2 strings up to the first NUL in each, ignoring ASCII case.
 *
 * @param left First string.
 * @param right Second string.
 * @return True when the strings match up to their terminators.
 */
REX_API bool utf8_equal_case_z(const std::string_view left, const std::string_view right);

/**
 * Finds the first code point that matches any of a set.
 *
 * @param haystack Text to search.
 * @param needles Code points to find.
 * @return Byte offset of the match, 0 when needles is empty, or npos when nothing matches.
 */
REX_API std::string_view::size_type utf8_find_any_of(const std::string_view haystack,
                                                     const std::string_view needles);

/**
 * Finds the first code point that matches any of a set, ignoring ASCII case.
 *
 * @param haystack Text to search.
 * @param needles Code points to find.
 * @return Byte offset of the match, 0 when needles is empty, or npos when nothing matches.
 */
REX_API std::string_view::size_type utf8_find_any_of_case(const std::string_view haystack,
                                                          const std::string_view needles);

/**
 * Finds the first occurrence of a substring.
 *
 * @param haystack Text to search.
 * @param needle Substring to find.
 * @return Byte offset of the match, 0 when needle is empty, or npos when nothing matches.
 */
REX_API std::string_view::size_type utf8_find_first_of(const std::string_view haystack,
                                                       const std::string_view needle);

/**
 * Finds the first occurrence of a substring, ignoring ASCII case.
 *
 * @param haystack Text to search.
 * @param needle Substring to find.
 * @return Byte offset of the match, 0 when needle is empty, or npos when nothing matches.
 */
REX_API std::string_view::size_type utf8_find_first_of_case(const std::string_view haystack,
                                                            const std::string_view needle);

/**
 * Reports whether a string starts with a prefix.
 *
 * @param haystack Text to test.
 * @param needle Prefix.
 * @return True when haystack starts with needle or needle is empty.
 */
REX_API bool utf8_starts_with(const std::string_view haystack, const std::string_view needle);

/**
 * Reports whether a string starts with a prefix, ignoring ASCII case.
 *
 * @param haystack Text to test.
 * @param needle Prefix.
 * @return True when haystack starts with needle or needle is empty.
 */
REX_API bool utf8_starts_with_case(const std::string_view haystack, const std::string_view needle);

/**
 * Reports whether a string ends with a suffix.
 *
 * @param haystack Text to test.
 * @param needle Suffix.
 * @return True when haystack ends with needle or needle is empty.
 */
REX_API bool utf8_ends_with(const std::string_view haystack, const std::string_view needle);

/**
 * Reports whether a string ends with a suffix, ignoring ASCII case.
 *
 * @param haystack Text to test.
 * @param needle Suffix.
 * @return True when haystack ends with needle or needle is empty.
 */
REX_API bool utf8_ends_with_case(const std::string_view haystack, const std::string_view needle);

/**
 * Splits a path on both / and \ and drops empty parts.
 *
 * @param path Path to split.
 * @return Views of the path components, in order.
 */
REX_API std::vector<std::string_view> utf8_split_path(const std::string_view path);

/**
 * Joins 2 path segments, adding a separator unless the left segment already ends with 1.
 *
 * @param left_path First segment. When empty the result is right_path.
 * @param right_path Second segment. When empty the result is left_path.
 * @param separator Separator to insert.
 * @return The joined path.
 */
REX_API std::string utf8_join_paths(const std::string_view left_path,
                                    const std::string_view right_path,
                                    char32_t separator = kUTF8PathSeparator);

/**
 * Joins path segments in order.
 *
 * @param paths Segments to join.
 * @param separator Separator to insert.
 * @return The joined path.
 */
REX_API std::string utf8_join_paths(const std::vector<std::string>& paths,
                                    char32_t separator = kUTF8PathSeparator);

/**
 * Joins path segments in order.
 *
 * @param paths Segments to join.
 * @param separator Separator to insert.
 * @return The joined path.
 */
REX_API std::string utf8_join_paths(const std::vector<std::string_view>& paths,
                                    char32_t separator = kUTF8PathSeparator);

/**
 * Joins path segments in order.
 *
 * @param paths Segments to join.
 * @param separator Separator to insert.
 * @return The joined path.
 */
inline std::string utf8_join_paths(std::initializer_list<const std::string_view> paths,
                                   char32_t separator = kUTF8PathSeparator) {
  std::string result;
  for (auto path : paths) {
    result = utf8_join_paths(result, path, separator);
  }
  return result;
}

/**
 * Joins 2 guest path segments with a backslash.
 *
 * @param left_path First segment.
 * @param right_path Second segment.
 * @return The joined path.
 */
inline std::string utf8_join_guest_paths(const std::string_view left_path,
                                         const std::string_view right_path) {
  return utf8_join_paths(left_path, right_path, kUTF8GuestPathSeparator);
}

/**
 * Joins guest path segments in order with backslashes.
 *
 * @param paths Segments to join.
 * @return The joined path.
 */
inline std::string utf8_join_guest_paths(const std::vector<std::string>& paths) {
  return utf8_join_paths(paths, kUTF8GuestPathSeparator);
}

/**
 * Joins guest path segments in order with backslashes.
 *
 * @param paths Segments to join.
 * @return The joined path.
 */
inline std::string utf8_join_guest_paths(const std::vector<std::string_view>& paths) {
  return utf8_join_paths(paths, kUTF8GuestPathSeparator);
}

/**
 * Joins guest path segments in order with backslashes.
 *
 * @param paths Segments to join.
 * @return The joined path.
 */
inline std::string utf8_join_guest_paths(std::initializer_list<const std::string_view> paths) {
  return utf8_join_paths(paths, kUTF8GuestPathSeparator);
}

/**
 * Replaces every / and \ with 1 separator and collapses repeated separators.
 *
 * @param path Path to rewrite.
 * @param new_separator Separator to use.
 * @return The rewritten path.
 */
REX_API std::string utf8_fix_path_separators(const std::string_view path,
                                             char32_t new_separator = kUTF8PathSeparator);

/**
 * Returns the last component of a path, ignoring trailing separators.
 *
 * @param path Path to read.
 * @param separator Path separator.
 * @return The file or directory name, or an empty string for an empty path or only separators.
 */
REX_API std::string utf8_find_name_from_path(const std::string_view path,
                                             char32_t separator = kUTF8PathSeparator);

/**
 * Returns the last component of a guest path, ignoring trailing backslashes.
 *
 * @param path Path to read.
 * @return The file or directory name.
 */
inline std::string utf8_find_name_from_guest_path(const std::string_view path) {
  return utf8_find_name_from_path(path, kUTF8GuestPathSeparator);
}

/**
 * Returns the last component of a path without its last extension.
 *
 * @param path Path to read.
 * @param separator Path separator.
 * @return The name up to its last dot. A name whose only dot is its first character is kept
 *         whole.
 */
REX_API std::string utf8_find_base_name_from_path(const std::string_view path,
                                                  char32_t separator = kUTF8PathSeparator);

/**
 * Returns the last component of a guest path without its last extension.
 *
 * @param path Path to read.
 * @return The name up to its last dot.
 */
inline std::string utf8_find_base_name_from_guest_path(const std::string_view path) {
  return utf8_find_base_name_from_path(path, kUTF8GuestPathSeparator);
}

/**
 * Returns the parent of a path. A parent that ends in a colon keeps its trailing separator.
 *
 * @param path Path to read.
 * @param separator Path separator.
 * @return The parent path, or an empty string when the path has no separator.
 */
REX_API std::string utf8_find_base_path(const std::string_view path,
                                        char32_t separator = kUTF8PathSeparator);

/**
 * Returns the parent of a guest path.
 *
 * @param path Path to read.
 * @return The parent path, or an empty string when the path has no backslash.
 */
inline std::string utf8_find_base_guest_path(const std::string_view path) {
  return utf8_find_base_path(path, kUTF8GuestPathSeparator);
}

/**
 * Removes . and .. components from a path. A .. never removes a component that ends in a colon.
 * Both / and \ split components, and the result uses 1 separator.
 *
 * @param path Path to canonicalize.
 * @param separator Separator of the result.
 * @return The canonical path, with a leading separator when path starts with 1.
 */
REX_API std::string utf8_canonicalize_path(const std::string_view path,
                                           char32_t separator = kUTF8PathSeparator);

/**
 * Removes . and .. components from a guest path and joins it with backslashes.
 *
 * @param path Path to canonicalize.
 * @return The canonical guest path.
 */
inline std::string utf8_canonicalize_guest_path(const std::string_view path) {
  return utf8_canonicalize_path(path, kUTF8GuestPathSeparator);
}

}  // namespace rex
