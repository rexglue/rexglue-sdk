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
 * @changes     - Flattened rex::string::utf8_ namespace to rex::string::
 *              - Added utf8_ prefix to all functions
 */

#pragma once

#include <string>
#include <vector>

#include <rex/platform.h>

namespace rex::string {

// UTF-8 path separator constants
constexpr char32_t kUtf8PathSeparator = static_cast<char32_t>(rex::kPathSeparator);
constexpr char32_t kUtf8GuestPathSeparator = U'\\';

size_t utf8_count(const std::string_view view);

std::string utf8_lower_ascii(const std::string_view view);
std::string utf8_upper_ascii(const std::string_view view);

size_t utf8_hash_fnv1a(const std::string_view view);
size_t utf8_hash_fnv1a_case(const std::string_view view);

// Splits the given haystack on any delimiters (needles) and returns all parts.
std::vector<std::string_view> utf8_split(const std::string_view haystack,
                                         const std::string_view needles, bool remove_empty = false);

bool utf8_equal_z(const std::string_view left, const std::string_view right);

bool utf8_equal_case(const std::string_view left, const std::string_view right);

bool utf8_equal_case_z(const std::string_view left, const std::string_view right);

std::string_view::size_type utf8_find_any_of(const std::string_view haystack,
                                             const std::string_view needles);

std::string_view::size_type utf8_find_any_of_case(const std::string_view haystack,
                                                  const std::string_view needles);

std::string_view::size_type utf8_find_first_of(const std::string_view haystack,
                                               const std::string_view needle);

// find_first_of string, case insensitive.
std::string_view::size_type utf8_find_first_of_case(const std::string_view haystack,
                                                    const std::string_view needle);

bool utf8_starts_with(const std::string_view haystack, const std::string_view needle);

bool utf8_starts_with_case(const std::string_view haystack, const std::string_view needle);

bool utf8_ends_with(const std::string_view haystack, const std::string_view needle);

bool utf8_ends_with_case(const std::string_view haystack, const std::string_view needle);

// Splits the given path on any valid path separator and returns all parts.
std::vector<std::string_view> utf8_split_path(const std::string_view path);

// Joins two path segments with the given separator.
std::string utf8_join_paths(const std::string_view left_path, const std::string_view right_path,
                            char32_t separator = kUtf8PathSeparator);

std::string utf8_join_paths(const std::vector<std::string>& paths,
                            char32_t separator = kUtf8PathSeparator);

std::string utf8_join_paths(const std::vector<std::string_view>& paths,
                            char32_t separator = kUtf8PathSeparator);

inline std::string utf8_join_paths(std::initializer_list<const std::string_view> paths,
                                   char32_t separator = kUtf8PathSeparator) {
  std::string result;
  for (auto path : paths) {
    result = utf8_join_paths(result, path, separator);
  }
  return result;
}

inline std::string utf8_join_guest_paths(const std::string_view left_path,
                                         const std::string_view right_path) {
  return utf8_join_paths(left_path, right_path, kUtf8GuestPathSeparator);
}

inline std::string utf8_join_guest_paths(const std::vector<std::string>& paths) {
  return utf8_join_paths(paths, kUtf8GuestPathSeparator);
}

inline std::string utf8_join_guest_paths(const std::vector<std::string_view>& paths) {
  return utf8_join_paths(paths, kUtf8GuestPathSeparator);
}

inline std::string utf8_join_guest_paths(std::initializer_list<const std::string_view> paths) {
  return utf8_join_paths(paths, kUtf8GuestPathSeparator);
}

// Replaces all path separators with the given value and removes redundant
// separators.
std::string utf8_fix_path_separators(const std::string_view path,
                                     char32_t new_separator = kUtf8PathSeparator);

inline std::string utf8_fix_guest_path_separators(const std::string_view path) {
  return utf8_fix_path_separators(path, kUtf8GuestPathSeparator);
}

// Find the top directory name or filename from a path.
std::string utf8_find_name_from_path(const std::string_view path,
                                     char32_t separator = kUtf8PathSeparator);

inline std::string utf8_find_name_from_guest_path(const std::string_view path) {
  return utf8_find_name_from_path(path, kUtf8GuestPathSeparator);
}

std::string utf8_find_base_name_from_path(const std::string_view path,
                                          char32_t separator = kUtf8PathSeparator);

inline std::string utf8_find_base_name_from_guest_path(const std::string_view path) {
  return utf8_find_base_name_from_path(path, kUtf8GuestPathSeparator);
}

// Get parent path of the given directory or filename.
std::string utf8_find_base_path(const std::string_view path,
                                char32_t separator = kUtf8PathSeparator);

inline std::string utf8_find_base_guest_path(const std::string_view path) {
  return utf8_find_base_path(path, kUtf8GuestPathSeparator);
}

// Canonicalizes a path, removing ..'s.
std::string utf8_canonicalize_path(const std::string_view path,
                                   char32_t separator = kUtf8PathSeparator);

inline std::string utf8_canonicalize_guest_path(const std::string_view path) {
  return utf8_canonicalize_path(path, kUtf8GuestPathSeparator);
}

}  // namespace rex::string
