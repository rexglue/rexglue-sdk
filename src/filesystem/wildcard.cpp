/**
 * @file        filesystem/wildcard.cpp
 * @brief       Case insensitive Xbox wildcard matching of directory entry names
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

#include <rex/string.h>

namespace rex::filesystem {

WildcardMatcher::WildcardMatcher(std::string_view pattern) {
  bool from_start = true;
  size_t last = 0;
  size_t n = 0;
  while ((n = pattern.find_first_of("*?", last)) != std::string_view::npos) {
    if (last != n) {
      rules_.push_back(
          {utf8_lower_ascii(pattern.substr(last, n - last)), from_start, false, false});
    }
    if (pattern[n] == '?') {
      const size_t end = pattern.find_first_not_of('?', n + 1);
      const size_t count = end == std::string_view::npos ? pattern.size() - n : end - n;
      rules_.push_back({std::string(pattern.substr(n, count)), false, false, true});
      last = n + count;
    } else {
      last = n + 1;
    }
    from_start = false;
  }
  if (last != pattern.size()) {
    rules_.push_back({utf8_lower_ascii(pattern.substr(last)), last == 0, true, false});
  }
}

bool WildcardMatcher::Check(const Rule& rule, std::string_view lower, size_t* offset) {
  if (rule.text.empty()) {
    return true;
  }
  if (lower.size() - *offset < rule.text.size()) {
    return false;
  }
  if (rule.exact_length) {
    *offset += rule.text.size();
    return true;
  }
  const size_t found = lower.find(rule.text, *offset);
  if (found == std::string_view::npos) {
    return false;
  }
  if (rule.from_start && found != *offset) {
    return false;
  }
  if (rule.to_end && found != lower.size() - rule.text.size()) {
    return false;
  }
  *offset = found + rule.text.size();
  return true;
}

bool WildcardMatcher::Match(std::string_view name) const {
  const std::string lower = utf8_lower_ascii(name);
  size_t offset = 0;
  for (const Rule& rule : rules_) {
    if (!Check(rule, lower, &offset)) {
      return false;
    }
  }
  return true;
}

}  // namespace rex::filesystem
