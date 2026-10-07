/**
 * @file        filesystem/wildcard.h
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

namespace rex::filesystem {

/**
 * Matches names against an NT file name expression.
 *
 * A star is any run and a question mark is 1 character. The DOS star `<` is any run up to the
 * final dot, the DOS question mark `>` is 1 character or nothing before a dot or the end, and the
 * DOS dot `"` is a dot or the end. The pattern `*.*` matches every name.
 */
class WildcardMatcher {
 public:
  /**
   * Compiles a pattern.
   *
   * @param pattern The pattern, empty to match every name.
   */
  explicit WildcardMatcher(std::string_view pattern = {});

  /**
   * Reports whether a name matches, ignoring ASCII case.
   *
   * @param name The name to test.
   * @return True when the name matches.
   */
  [[nodiscard]] bool Match(std::string_view name) const;

 private:
  std::string pattern_;
};

}  // namespace rex::filesystem
