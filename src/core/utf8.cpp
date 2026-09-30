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

#include <algorithm>
#include <cstdint>
#include <locale>
#include <numeric>
#include <tuple>

#include <rex/string/utf8.h>

#define UTF_CPP_CPLUSPLUS 201703L
#include <utf8.h>

namespace utfcpp = utf8;

using UTF8Iterator = utfcpp::iterator<std::string_view::const_iterator>;

namespace rex {

std::string to_utf8(const std::u16string_view source) {
  return utfcpp::utf16to8(source);
}

std::u16string to_utf16(const std::string_view source) {
  return utfcpp::utf8to16(source);
}

namespace {
std::string ToString(char32_t c) {
  std::string result;
  utfcpp::append(c, result);
  return result;
}

u32 LowerASCII(const u32 c) {
  return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

u32 UpperASCII(const u32 c) {
  return c >= 'a' && c <= 'z' ? c - 32 : c;
}

bool EqualASCIICase(const u32 l, const u32 r) {
  return l == r || LowerASCII(l) == LowerASCII(r);
}

std::pair<UTF8Iterator, UTF8Iterator> MakeIterators(const std::string_view view) {
  return {UTF8Iterator(view.cbegin(), view.cbegin(), view.cend()),
          UTF8Iterator(view.cend(), view.cbegin(), view.cend())};
}

std::pair<UTF8Iterator, UTF8Iterator> MakeIterators(const UTF8Iterator begin,
                                                    const UTF8Iterator end) {
  return {UTF8Iterator(begin.base(), begin.base(), end.base()),
          UTF8Iterator(end.base(), begin.base(), end.base())};
}

size_t ByteLength(UTF8Iterator begin, UTF8Iterator end) {
  return size_t(std::distance(begin.base(), end.base()));
}
}  // namespace

size_t utf8_count(const std::string_view view) {
  return size_t(utfcpp::distance(view.cbegin(), view.cend()));
}

std::string utf8_lower_ascii(const std::string_view view) {
  auto [begin, end] = MakeIterators(view);
  std::string result;
  for (auto it = begin; it != end; ++it) {
    utfcpp::append(char32_t(LowerASCII(*it)), result);
  }
  return result;
}

std::string utf8_upper_ascii(const std::string_view view) {
  auto [begin, end] = MakeIterators(view);
  std::string result;
  for (auto it = begin; it != end; ++it) {
    utfcpp::append(char32_t(UpperASCII(*it)), result);
  }
  return result;
}

namespace {
template <bool LOWER>
inline size_t HashFNV1a(const std::string_view view) {
  const size_t offset_basis = 0xCBF29CE484222325ull;
  const size_t prime = 0x00000100000001B3ull;
  auto work = [&prime](size_t hash, u8 byte_of_data) {
    hash ^= byte_of_data;
    hash *= prime;
    return hash;
  };
  auto hash = offset_basis;
  auto [begin, end] = MakeIterators(view);
  for (auto it = begin; it != end; ++it) {
    u32 c;
    if constexpr (LOWER) {
      c = LowerASCII(*it);
    } else {
      c = *it;
    }
    hash = work(hash, u8((c >> 0) & 0xFF));
    hash = work(hash, u8((c >> 8) & 0xFF));
    hash = work(hash, u8((c >> 16) & 0xFF));
    hash = work(hash, u8((c >> 24) & 0xFF));
  }
  return hash;
}
}  // namespace

size_t utf8_hash_fnv1a(const std::string_view view) {
  return HashFNV1a<false>(view);
}

size_t utf8_hash_fnv1a_case(const std::string_view view) {
  return HashFNV1a<true>(view);
}

namespace {
inline UTF8Iterator FindNeedle(UTF8Iterator haystack_it, UTF8Iterator haystack_end,
                               UTF8Iterator needle_begin, UTF8Iterator needle_end) {
  return std::find_if(haystack_it, haystack_end, [&](const auto& c) {
    for (auto needle = needle_begin; needle != needle_end; ++needle) {
      if (c == *needle) {
        return true;
      }
    }
    return false;
  });
}

inline UTF8Iterator FindNeedleCase(UTF8Iterator haystack_it, UTF8Iterator haystack_end,
                                   UTF8Iterator needle_begin, UTF8Iterator needle_end) {
  return std::find_if(haystack_it, haystack_end, [&](const auto& c) {
    for (auto needle = needle_begin; needle != needle_end; ++needle) {
      if (EqualASCIICase(c, *needle)) {
        return true;
      }
    }
    return false;
  });
}
}  // namespace

std::vector<std::string_view> utf8_split(const std::string_view haystack,
                                         const std::string_view needles, bool remove_empty) {
  std::vector<std::string_view> result;

  auto [haystack_begin, haystack_end] = MakeIterators(haystack);
  auto [needle_begin, needle_end] = MakeIterators(needles);

  auto it = haystack_begin;
  auto last = it;
  for (;;) {
    it = FindNeedle(it, haystack_end, needle_begin, needle_end);
    if (it == haystack_end) {
      break;
    }

    if (it != last) {
      auto offset = ByteLength(haystack_begin, last);
      auto length = ByteLength(haystack_begin, it) - offset;
      result.push_back(haystack.substr(offset, length));
    } else if (!remove_empty) {
      result.push_back("");
    }

    ++it;
    last = it;
  }

  if (last != haystack_end) {
    auto offset = ByteLength(haystack_begin, last);
    result.push_back(haystack.substr(offset));
  }

  return result;
}

bool utf8_equal_z(const std::string_view left, const std::string_view right) {
  if (left.empty()) {
    return right.empty();
  } else if (right.empty()) {
    return false;
  }
  auto [left_begin, left_end] = MakeIterators(left);
  auto [right_begin, right_end] = MakeIterators(right);
  auto left_it = left_begin;
  auto right_it = right_begin;
  for (; left_it != left_end && *left_it != 0 && right_it != right_end && *right_it != 0;
       ++left_it, ++right_it) {
    if (*left_it != *right_it) {
      return false;
    }
  }
  return (left_it == left_end || *left_it == 0) && (right_it == right_end || *right_it == 0);
}

bool utf8_equal_case(const std::string_view left, const std::string_view right) {
  if (left.empty()) {
    return right.empty();
  } else if (right.empty()) {
    return false;
  }
  auto [left_begin, left_end] = MakeIterators(left);
  auto [right_begin, right_end] = MakeIterators(right);
  return std::equal(left_begin, left_end, right_begin, right_end, EqualASCIICase);
}

bool utf8_equal_case_z(const std::string_view left, const std::string_view right) {
  if (left.empty()) {
    return right.empty();
  } else if (right.empty()) {
    return false;
  }
  auto [left_begin, left_end] = MakeIterators(left);
  auto [right_begin, right_end] = MakeIterators(right);
  auto left_it = left_begin;
  auto right_it = right_begin;
  for (; left_it != left_end && *left_it != 0 && right_it != right_end && *right_it != 0;
       ++left_it, ++right_it) {
    if (!EqualASCIICase(*left_it, *right_it)) {
      return false;
    }
  }
  return (left_it == left_end || *left_it == 0) && (right_it == right_end || *right_it == 0);
}

std::string_view::size_type utf8_find_any_of(const std::string_view haystack,
                                             const std::string_view needles) {
  if (needles.empty()) {
    return std::string_view::size_type(0);
  } else if (haystack.empty()) {
    return std::string_view::npos;
  }

  auto [haystack_begin, haystack_end] = MakeIterators(haystack);
  auto [needle_begin, needle_end] = MakeIterators(needles);

  auto it = FindNeedle(haystack_begin, haystack_end, needle_begin, needle_end);
  if (it == haystack_end) {
    return std::string_view::npos;
  }

  return std::string_view::size_type(ByteLength(haystack_begin, it));
}

std::string_view::size_type utf8_find_any_of_case(const std::string_view haystack,
                                                  const std::string_view needles) {
  if (needles.empty()) {
    return std::string_view::size_type(0);
  } else if (haystack.empty()) {
    return std::string_view::npos;
  }

  auto [haystack_begin, haystack_end] = MakeIterators(haystack);
  auto [needle_begin, needle_end] = MakeIterators(needles);

  auto it = FindNeedleCase(haystack_begin, haystack_end, needle_begin, needle_end);
  if (it == haystack_end) {
    return std::string_view::npos;
  }

  return std::string_view::size_type(ByteLength(haystack_begin, it));
}

std::string_view::size_type utf8_find_first_of(const std::string_view haystack,
                                               const std::string_view needle) {
  if (needle.empty()) {
    return std::string_view::size_type(0);
  } else if (haystack.empty()) {
    return std::string_view::npos;
  }

  auto [haystack_begin, haystack_end] = MakeIterators(haystack);
  auto [needle_begin, needle_end] = MakeIterators(needle);
  auto needle_count = utf8_count(needle);

  auto it = haystack_begin;
  for (; it != haystack_end; ++it) {
    it = std::find(it, haystack_end, *needle_begin);
    if (it == haystack_end) {
      return std::string_view::npos;
    }

    auto end = it;
    for (size_t i = 0; i < needle_count; ++i) {
      if (end == haystack_end) {
        return std::string_view::npos;
      }
      ++end;
    }

    auto [sub_start, sub_end] = MakeIterators(it, end);
    if (std::equal(needle_begin, needle_end, sub_start, sub_end)) {
      return std::string_view::size_type(ByteLength(haystack_begin, it));
    }
  }

  return std::string_view::npos;
}

std::string_view::size_type utf8_find_first_of_case(const std::string_view haystack,
                                                    const std::string_view needle) {
  if (needle.empty()) {
    return std::string_view::size_type(0);
  } else if (haystack.empty()) {
    return std::string_view::npos;
  }

  auto [haystack_begin, haystack_end] = MakeIterators(haystack);
  auto [needle_begin, needle_end] = MakeIterators(needle);
  auto needle_count = utf8_count(needle);
  auto nc = *needle_begin;

  auto it = haystack_begin;
  for (; it != haystack_end; ++it) {
    it = std::find_if(it, haystack_end, [&nc](const u32& c) { return EqualASCIICase(nc, c); });
    if (it == haystack_end) {
      return std::string_view::npos;
    }

    auto end = it;
    for (size_t i = 0; i < needle_count; ++i) {
      if (end == haystack_end) {
        return std::string_view::npos;
      }
      ++end;
    }

    auto [sub_start, sub_end] = MakeIterators(it, end);
    if (std::equal(needle_begin, needle_end, sub_start, sub_end, EqualASCIICase)) {
      return std::string_view::size_type(ByteLength(haystack_begin, it));
    }
  }

  return std::string_view::npos;
}

namespace {
bool StartsWithCodePoint(const std::string_view haystack, char32_t needle) {
  if (haystack.empty()) {
    return false;
  }
  auto [it, end] = MakeIterators(haystack);
  return *it == u32(needle);
}
}  // namespace

bool utf8_starts_with(const std::string_view haystack, const std::string_view needle) {
  if (needle.empty()) {
    return true;
  } else if (haystack.empty()) {
    return false;
  }

  auto [haystack_begin, haystack_end] = MakeIterators(haystack);
  auto [needle_begin, needle_end] = MakeIterators(needle);
  auto needle_count = utf8_count(needle);

  auto it = haystack_begin;
  auto end = it;
  for (size_t i = 0; i < needle_count; ++i) {
    if (end == haystack_end) {
      return false;
    }
    ++end;
  }

  auto [sub_start, sub_end] = MakeIterators(it, end);
  return std::equal(needle_begin, needle_end, sub_start, sub_end);
}

bool utf8_starts_with_case(const std::string_view haystack, const std::string_view needle) {
  if (needle.empty()) {
    return true;
  } else if (haystack.empty()) {
    return false;
  }

  auto [haystack_begin, haystack_end] = MakeIterators(haystack);
  auto [needle_begin, needle_end] = MakeIterators(needle);
  auto needle_count = utf8_count(needle);

  auto it = haystack_begin;
  auto end = it;
  for (size_t i = 0; i < needle_count; ++i) {
    if (end == haystack_end) {
      return false;
    }
    ++end;
  }

  auto [sub_start, sub_end] = MakeIterators(it, end);
  return std::equal(needle_begin, needle_end, sub_start, sub_end, EqualASCIICase);
}

bool utf8_ends_with(const std::string_view haystack, const std::string_view needle) {
  if (needle.empty()) {
    return true;
  } else if (haystack.empty()) {
    return false;
  }

  auto [haystack_begin, haystack_end] = MakeIterators(haystack);
  auto [needle_begin, needle_end] = MakeIterators(needle);
  auto needle_count = utf8_count(needle);

  auto it = haystack_end;
  auto end = it;
  --it;

  for (size_t i = 1; i < needle_count; ++i) {
    if (it == haystack_begin) {
      return false;
    }
    --it;
  }

  auto [sub_start, sub_end] = MakeIterators(it, end);
  return std::equal(needle_begin, needle_end, sub_start, sub_end);
}

bool utf8_ends_with_case(const std::string_view haystack, const std::string_view needle) {
  if (needle.empty()) {
    return true;
  } else if (haystack.empty()) {
    return false;
  }

  auto [haystack_begin, haystack_end] = MakeIterators(haystack);
  auto [needle_begin, needle_end] = MakeIterators(needle);
  auto needle_count = utf8_count(needle);

  auto it = haystack_end;
  auto end = it;
  --it;

  for (size_t i = 1; i < needle_count; ++i) {
    if (it == haystack_begin) {
      return false;
    }
    --it;
  }

  auto [sub_start, sub_end] = MakeIterators(it, end);
  return std::equal(needle_begin, needle_end, sub_start, sub_end, EqualASCIICase);
}

std::vector<std::string_view> utf8_split_path(const std::string_view path) {
  return utf8_split(path, "\\/", true);
}

std::string utf8_join_paths(const std::string_view left_path, const std::string_view right_path,
                            char32_t separator) {
  if (left_path.empty()) {
    return std::string(right_path);
  } else if (right_path.empty()) {
    return std::string(left_path);
  }

  UTF8Iterator it;
  std::tie(std::ignore, it) = MakeIterators(left_path);
  --it;

  std::string result = std::string(left_path);
  if (*it != static_cast<u32>(separator)) {
    utfcpp::append(separator, result);
  }
  return result + std::string(right_path);
}

std::string utf8_join_paths(const std::vector<std::string>& paths, char32_t separator) {
  std::string result;
  auto it = paths.cbegin();
  if (it != paths.cend()) {
    result = *it++;
    for (; it != paths.cend(); ++it) {
      result = utf8_join_paths(result, *it, separator);
    }
  }
  return result;
}

std::string utf8_join_paths(const std::vector<std::string_view>& paths, char32_t separator) {
  std::string result;
  auto it = paths.cbegin();
  if (it != paths.cend()) {
    result = *it++;
    for (; it != paths.cend(); ++it) {
      result = utf8_join_paths(result, *it, separator);
    }
  }
  return result;
}

std::string utf8_fix_path_separators(const std::string_view path, char32_t new_separator) {
  if (path.empty()) {
    return std::string();
  }

  const char32_t old_separator = new_separator == U'\\' ? U'/' : U'\\';

  auto [path_begin, path_end] = MakeIterators(path);

  std::string result;
  auto it = path_begin;
  auto last = it;

  auto is_separator = [old_separator, new_separator](char32_t c) {
    return c == u32(old_separator) || c == u32(new_separator);
  };

  if (is_separator(*it)) {
    utfcpp::append(new_separator, result);
    ++it;
    last = it;
  }

  for (;;) {
    it = std::find_if(it, path_end, is_separator);
    if (it == path_end) {
      break;
    }

    if (it != last) {
      auto offset = ByteLength(path_begin, last);
      auto length = ByteLength(path_begin, it) - offset;
      result += path.substr(offset, length);
      utfcpp::append(new_separator, result);
    }

    ++it;
    last = it;
  }

  if (last == path_begin) {
    return std::string(path);
  }

  if (last != path_end) {
    auto offset = ByteLength(path_begin, last);
    result += path.substr(offset);
  }

  return result;
}

std::string utf8_find_name_from_path(const std::string_view path, char32_t separator) {
  if (path.empty()) {
    return std::string();
  }

  auto [begin, end] = MakeIterators(path);

  auto it = end;
  --it;

  while (*it == u32(separator)) {
    if (it == begin) {
      return std::string();
    }
    --it;
  }

  end = std::next(it);

  while (*it != u32(separator)) {
    if (it == begin) {
      break;
    }
    --it;
  }

  if (*it == u32(separator)) {
    ++it;
  }

  auto offset = ByteLength(begin, it);
  auto length = ByteLength(it, end);
  return std::string(path.substr(offset, length));
}

std::string utf8_find_base_name_from_path(const std::string_view path, char32_t separator) {
  auto name = utf8_find_name_from_path(path, separator);
  if (name.empty()) {
    return std::string();
  }

  auto [begin, end] = MakeIterators(name);

  auto it = end;
  --it;

  while (it != begin) {
    if (*it == u32('.')) {
      break;
    }
    --it;
  }

  if (it == begin) {
    return name;
  }

  auto length = ByteLength(it, end);
  auto offset = name.length() - length;
  return std::string(name.substr(0, offset));
}

std::string utf8_find_base_path(const std::string_view path, char32_t separator) {
  if (path.empty()) {
    return std::string();
  }

  auto [begin, end] = MakeIterators(path);

  auto it = end;
  --it;

  while (*it == u32(separator)) {
    if (it == begin) {
      return std::string();
    }
    --it;
  }

  while (*it != u32(separator)) {
    if (it == begin) {
      return std::string();
    }
    --it;
  }

  auto sep_pos = it;

  while (*it == u32(separator)) {
    if (it == begin) {
      return std::string();
    }
    --it;
  }

  // A drive letter such as "D:" keeps its root separator.
  if (*it == u32(':')) {
    auto length = ByteLength(begin, std::next(sep_pos));
    return std::string(path.substr(0, length));
  }

  auto length = ByteLength(begin, std::next(it));
  return std::string(path.substr(0, length));
}

std::string utf8_canonicalize_path(const std::string_view path, char32_t separator) {
  if (path.empty()) {
    return std::string();
  }

  auto is_rooted = StartsWithCodePoint(path, separator);

  auto parts = utf8_split_path(path);
  for (auto it = parts.begin(); it != parts.end();) {
    const auto& part = *it;
    if (part == ".") {
      it = parts.erase(it);
    } else if (part == "..") {
      // ".." never removes a device name such as "game:".
      if (it != parts.begin()) {
        auto prev = std::prev(it);
        if (!utf8_ends_with(*prev, ":")) {
          it = parts.erase(prev);
        }
      }
      it = parts.erase(it);
    } else {
      ++it;
    }
  }

  return !is_rooted ? utf8_join_paths(parts, separator)
                    : ToString(separator) + utf8_join_paths(parts, separator);
}

}  // namespace rex
