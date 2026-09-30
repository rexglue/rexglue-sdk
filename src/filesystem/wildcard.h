/**
 * @file        filesystem/wildcard.h
 * @brief       Case insensitive Xbox wildcard matching of directory entry names
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/filesystem_wildcard.h (Ben Vanik, 2020).
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace rex::filesystem {

/** Matches names against a pattern where a star is any run and a question mark is 1 character. */
class WildcardMatcher {
 public:
  /**
   * Compiles a pattern.
   *
   * @param pattern The pattern, empty to match every name.
   */
  explicit WildcardMatcher(std::string_view pattern = {});

  /**
   * Tests whether a name matches, ignoring ASCII case.
   *
   * @param name The name to test.
   * @return True when the name matches.
   */
  [[nodiscard]] bool Match(std::string_view name) const;

 private:
  struct Rule {
    std::string text;
    bool from_start = false;
    bool to_end = false;
    bool exact_length = false;
  };

  static bool Check(const Rule& rule, std::string_view lower, size_t* offset);

  std::vector<Rule> rules_;
};

}  // namespace rex::filesystem
