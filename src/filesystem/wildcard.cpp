#include "filesystem/wildcard.h"

#include <algorithm>
#include <vector>

#include <rex/string/text.h>

namespace rex::filesystem {

WildcardMatcher::WildcardMatcher(std::string_view pattern)
    : pattern_(pattern == "*.*" ? std::string("*") : std::string(pattern)) {}

bool WildcardMatcher::Match(std::string_view name) const {
  if (pattern_.empty()) {
    return true;
  }
  const size_t last_dot = name.rfind('.');
  const size_t count = pattern_.size();
  std::vector<bool> active(count + 1);
  std::vector<bool> next(count + 1);
  active[0] = true;
  for (size_t n = 0;; ++n) {
    const bool at_end = n == name.size();
    const char c = at_end ? '\0' : name[n];
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
          next[p + 1] = next[p + 1] || string::utf8_equal_case({&pattern_[p], 1}, {&c, 1});
          break;
      }
    }
    std::swap(active, next);
  }
}

}  // namespace rex::filesystem
