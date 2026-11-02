/**
 * @file        filesystem/path.h
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

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <rex/platform.h>
#include <rex/types.h>

namespace rex::filesystem {

constexpr char32_t kUTF8PathSeparator = static_cast<char32_t>(rex::platform::kPathSeparator);
constexpr char32_t kUTF8GuestPathSeparator = U'\\';

/**
 * Splits a path on both / and \ and omits empty parts.
 *
 * @return Views of the path components, in order.
 */
REX_API std::vector<std::string_view> utf8_split_path(const std::string_view path);

REX_API std::string_view utf8_strip_leading_slashes(const std::string_view path);

/**
 * Joins two path segments, adding a separator unless the left segment ends with one.
 *
 * @param left_path First segment. When empty the result is right_path.
 * @param right_path Second segment. When empty the result is left_path.
 */
REX_API std::string utf8_join_paths(const std::string_view left_path,
                                    const std::string_view right_path,
                                    char32_t separator = kUTF8PathSeparator);

REX_API std::string utf8_join_paths(const std::vector<std::string>& paths,
                                    char32_t separator = kUTF8PathSeparator);

REX_API std::string utf8_join_paths(const std::vector<std::string_view>& paths,
                                    char32_t separator = kUTF8PathSeparator);

inline std::string utf8_join_paths(std::initializer_list<const std::string_view> paths,
                                   char32_t separator = kUTF8PathSeparator) {
  std::string result;
  for (auto path : paths) {
    result = utf8_join_paths(result, path, separator);
  }
  return result;
}

inline std::string utf8_join_guest_paths(const std::string_view left_path,
                                         const std::string_view right_path) {
  return utf8_join_paths(left_path, right_path, kUTF8GuestPathSeparator);
}

inline std::string utf8_join_guest_paths(const std::vector<std::string>& paths) {
  return utf8_join_paths(paths, kUTF8GuestPathSeparator);
}

inline std::string utf8_join_guest_paths(const std::vector<std::string_view>& paths) {
  return utf8_join_paths(paths, kUTF8GuestPathSeparator);
}

inline std::string utf8_join_guest_paths(std::initializer_list<const std::string_view> paths) {
  return utf8_join_paths(paths, kUTF8GuestPathSeparator);
}

/** Replaces every / and \ with one separator and collapses repeated separators. */
REX_API std::string utf8_fix_path_separators(const std::string_view path,
                                             char32_t new_separator = kUTF8PathSeparator);

/**
 * Returns the last component of a path, ignoring trailing separators, or an empty string for an
 * empty path or only separators.
 */
REX_API std::string utf8_find_name_from_path(const std::string_view path,
                                             char32_t separator = kUTF8PathSeparator);

inline std::string utf8_find_name_from_guest_path(const std::string_view path) {
  return utf8_find_name_from_path(path, kUTF8GuestPathSeparator);
}

/**
 * Returns the last component of a path without its last extension. A name whose only dot is its
 * first character is kept whole.
 */
REX_API std::string utf8_find_base_name_from_path(const std::string_view path,
                                                  char32_t separator = kUTF8PathSeparator);

inline std::string utf8_find_base_name_from_guest_path(const std::string_view path) {
  return utf8_find_base_name_from_path(path, kUTF8GuestPathSeparator);
}

/**
 * Returns the parent of a path, or an empty string when the path has no separator. A parent that
 * ends in a colon keeps its trailing separator.
 */
REX_API std::string utf8_find_base_path(const std::string_view path,
                                        char32_t separator = kUTF8PathSeparator);

inline std::string utf8_find_base_guest_path(const std::string_view path) {
  return utf8_find_base_path(path, kUTF8GuestPathSeparator);
}

/**
 * Removes . and .. components from a path. A .. never removes a component that ends in a colon.
 *
 * @param path Path to canonicalize, split on / and \.
 */
REX_API std::string utf8_canonicalize_path(const std::string_view path,
                                           char32_t separator = kUTF8PathSeparator);

/** Normalizes a guest path to the lowercase key form with forward slashes and no device prefix. */
REX_API std::string NormalizeGuestPath(std::string_view path);

inline std::string utf8_canonicalize_guest_path(const std::string_view path) {
  return utf8_canonicalize_path(path, kUTF8GuestPathSeparator);
}

}  // namespace rex::filesystem
