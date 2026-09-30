/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <rex/macros.h>
#include <rex/string.h>

namespace rex {
namespace filesystem {

class REX_API WildcardFlags {
 public:
  bool FromStart : 1, ToEnd : 1, ExactLength : 1;

  constexpr WildcardFlags() noexcept : FromStart(false), ToEnd(false), ExactLength(false) {}
  constexpr WildcardFlags(bool start, bool end, bool exact_length) noexcept
      : FromStart(start), ToEnd(end), ExactLength(exact_length) {}

  static const WildcardFlags FIRST;
  static const WildcardFlags LAST;
  static const WildcardFlags ANY;
  static const WildcardFlags FIRST_AND_LAST;
};

class REX_API WildcardRule {
 public:
  WildcardRule(const std::string_view match, const WildcardFlags& flags);
  bool Check(const std::string_view lower, std::string_view::size_type* offset) const;

 private:
  std::string match_;
  WildcardFlags rules_;
};

class REX_API WildcardEngine {
 public:
  void SetRule(const std::string_view pattern);

  // Always ignoring case
  bool Match(const std::string_view str) const;

 private:
  std::vector<WildcardRule> rules_;
  void PreparePattern(const std::string_view pattern);
};

}  // namespace filesystem
}  // namespace rex
