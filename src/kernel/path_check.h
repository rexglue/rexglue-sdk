/**
 * @file        kernel/path_check.h
 * @brief       Private check of the characters of an NT path a title passes to an export
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string_view>

namespace rex::kernel {

/** What a path names, which decides the characters it may hold. */
enum class PathUse {
  kExisting, /**< A path being opened, which may hold a semicolon and an equals sign. */
  kPattern,  /**< A search pattern, which may also hold * and ?. */
  kNewName,  /**< A name being given, which reserves the semicolon and the equals sign. */
};

/**
 * Reports whether a path holds only characters a file name may use.
 *
 * A pattern may hold * and ?, and a * must be followed by a . because some titles loop forever on
 * other filters. A name being given also reserves the semicolon and the equals sign. A path being
 * opened does not, so an existing disc or package path never fails to open.
 *
 * @param path The path.
 * @param use What the path names.
 * @return True when no character is a control, a non ASCII or a reserved character.
 */
inline bool IsValidPath(std::string_view path, PathUse use) {
  bool got_asterisk = false;
  for (const char c : path) {
    if (c <= 31 || c >= 127) {
      return false;
    }
    if (got_asterisk) {
      if (c != '.') {
        return false;
      }
      got_asterisk = false;
    }
    switch (c) {
      case '"':
      case '+':
      case ',':
      case '<':
      case '>':
      case '|':
        return false;
      case ';':
      case '=':
        if (use == PathUse::kNewName) {
          return false;
        }
        break;
      case '*':
        if (use != PathUse::kPattern) {
          return false;
        }
        got_asterisk = true;
        break;
      case '?':
        if (use != PathUse::kPattern) {
          return false;
        }
        break;
      default:
        break;
    }
  }
  return true;
}

}  // namespace rex::kernel
