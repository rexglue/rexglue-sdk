/**
 * @file        filesystem/wildcard.cpp
 * @brief       Case insensitive wildcard matching of directory entry names
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/filesystem_wildcard.cc (Ben Vanik, 2020).
 */

#include "filesystem/wildcard.h"

#include <algorithm>
#include <vector>

#include <rex/string.h>

namespace rex::filesystem {

WildcardMatcher::WildcardMatcher(std::string_view pattern)
    : pattern_(pattern == "*.*" ? std::string("*") : utf8_lower_ascii(pattern)) {}

bool WildcardMatcher::Match(std::string_view name) const {
  if (pattern_.empty()) {
    return true;
  }
  const std::string lower = utf8_lower_ascii(name);
  const size_t last_dot = lower.rfind('.');
  const size_t count = pattern_.size();
  std::vector<bool> active(count + 1);
  std::vector<bool> next(count + 1);
  active[0] = true;
  for (size_t n = 0;; ++n) {
    const bool at_end = n == lower.size();
    const char c = at_end ? '\0' : lower[n];
    for (size_t p = 0; p < count; ++p) {
      const char token = pattern_[p];
      if (active[p] && (token == '*' || token == '<' || (token == '>' && (at_end || c == '.')) ||
                        (token == '"' && at_end))) {
        active[p + 1] = true;
      }
    }
    if (at_end) {
      return active[count];
    }
    std::ranges::fill(next, false);
    for (size_t p = 0; p < count; ++p) {
      if (!active[p]) {
        continue;
      }
      switch (pattern_[p]) {
        case '*':
          next[p] = true;
          break;
        case '<':
          next[p] = next[p] || c != '.' || n != last_dot;
          break;
        case '?':
          next[p + 1] = true;
          break;
        case '>':
          next[p + 1] = next[p + 1] || c != '.';
          break;
        case '"':
          next[p + 1] = next[p + 1] || c == '.';
          break;
        default:
          next[p + 1] = next[p + 1] || pattern_[p] == c;
          break;
      }
    }
    std::swap(active, next);
  }
}

}  // namespace rex::filesystem
