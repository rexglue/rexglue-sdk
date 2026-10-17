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

#include <cctype>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <locale>
#include <numeric>
#include <tuple>
#include <type_traits>

#include <rex/string/utf8.h>

namespace rex {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

bool IsContinuation(char c) {
  return (static_cast<u8>(c) & 0xC0) == 0x80;
}

bool IsEncodable(char32_t code_point) {
  return code_point <= 0x10FFFF && (code_point < 0xD800 || code_point > 0xDFFF);
}

char32_t DecodeUTF8(const char*& it, const char* end) {
  const auto lead = static_cast<u8>(*it++);
  if (lead < 0x80) {
    return lead;
  }
  u32 trail_count = 0;
  char32_t code_point = 0;
  u8 low = 0x80;
  u8 high = 0xBF;
  if (lead >= 0xC2 && lead <= 0xDF) {
    trail_count = 1;
    code_point = lead & 0x1F;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    trail_count = 2;
    code_point = lead & 0x0F;
    low = lead == 0xE0 ? 0xA0 : 0x80;
    high = lead == 0xED ? 0x9F : 0xBF;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    trail_count = 3;
    code_point = lead & 0x07;
    low = lead == 0xF0 ? 0x90 : 0x80;
    high = lead == 0xF4 ? 0x8F : 0xBF;
  } else {
    return kReplacement;
  }
  for (u32 i = 0; i < trail_count; ++i) {
    if (it == end) {
      return kReplacement;
    }
    const auto trail = static_cast<u8>(*it);
    if (trail < low || trail > high) {
      return kReplacement;
    }
    code_point = (code_point << 6) | (trail & 0x3F);
    low = 0x80;
    high = 0xBF;
    ++it;
  }
  return code_point;
}

void EncodeUTF8(char32_t code_point, std::string& out) {
  if (!IsEncodable(code_point)) {
    code_point = kReplacement;
  }
  if (code_point < 0x80) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

char32_t DecodeUTF16(const char16_t*& it, const char16_t* end) {
  const char16_t unit = *it++;
  if (unit < 0xD800 || unit > 0xDFFF) {
    return unit;
  }
  if (unit > 0xDBFF || it == end || *it < 0xDC00 || *it > 0xDFFF) {
    return kReplacement;
  }
  const char16_t trail = *it++;
  return 0x10000 + ((static_cast<char32_t>(unit) - 0xD800) << 10) +
         (static_cast<char32_t>(trail) - 0xDC00);
}

void EncodeUTF16(char32_t code_point, std::u16string& out) {
  if (!IsEncodable(code_point)) {
    code_point = kReplacement;
  }
  if (code_point < 0x10000) {
    out.push_back(static_cast<char16_t>(code_point));
    return;
  }
  code_point -= 0x10000;
  out.push_back(static_cast<char16_t>(0xD800 + (code_point >> 10)));
  out.push_back(static_cast<char16_t>(0xDC00 + (code_point & 0x3FF)));
}

class CodepointIterator {
 public:
  using iterator_category = std::bidirectional_iterator_tag;
  using value_type = u32;
  using difference_type = std::ptrdiff_t;
  using pointer = void;
  using reference = u32;

  CodepointIterator() = default;
  CodepointIterator(const char* position, const char* range_begin, const char* range_end)
      : position_(position), range_begin_(range_begin), range_end_(range_end) {}

  [[nodiscard]] const char* Base() const { return position_; }

  u32 operator*() const {
    const char* it = position_;
    return DecodeUTF8(it, range_end_);
  }

  CodepointIterator& operator++() {
    DecodeUTF8(position_, range_end_);
    return *this;
  }

  CodepointIterator operator++(int) {
    CodepointIterator previous = *this;
    ++*this;
    return previous;
  }

  CodepointIterator& operator--() {
    const char* start = position_ - 1;
    for (u32 i = 0; i < 3 && start > range_begin_ && IsContinuation(*start); ++i) {
      --start;
    }
    const char* probe = start;
    DecodeUTF8(probe, range_end_);
    position_ = probe == position_ ? start : position_ - 1;
    return *this;
  }

  CodepointIterator operator--(int) {
    CodepointIterator previous = *this;
    --*this;
    return previous;
  }

  bool operator==(const CodepointIterator& other) const { return position_ == other.position_; }

 private:
  const char* position_ = nullptr;
  const char* range_begin_ = nullptr;
  const char* range_end_ = nullptr;
};

}  // namespace

std::string ToUTF8(const std::u16string_view source) {
  std::string result;
  result.reserve(source.size());
  const char16_t* it = source.data();
  const char16_t* end = it + source.size();
  while (it != end) {
    EncodeUTF8(DecodeUTF16(it, end), result);
  }
  return result;
}

std::u16string ToUTF16(const std::string_view source) {
  std::u16string result;
  result.reserve(source.size());
  const char* it = source.data();
  const char* end = it + source.size();
  while (it != end) {
    EncodeUTF16(DecodeUTF8(it, end), result);
  }
  return result;
}

namespace {

constexpr bool kWidePaths = std::is_same_v<std::filesystem::path::value_type, wchar_t>;

}  // namespace

std::string PathToUTF8(const std::filesystem::path& path) {
  if constexpr (kWidePaths) {
    return ToUTF8(path.u16string());
  } else {
    return path.string();
  }
}

std::u16string PathToUTF16(const std::filesystem::path& path) {
  if constexpr (kWidePaths) {
    return path.u16string();
  } else {
    return ToUTF16(path.string());
  }
}

std::filesystem::path to_path(const std::string_view source) {
  if constexpr (kWidePaths) {
    return {ToUTF16(source)};
  } else {
    return {source};
  }
}

std::filesystem::path to_path(const std::u16string_view source) {
  if constexpr (kWidePaths) {
    return {source};
  } else {
    return {ToUTF8(source)};
  }
}

namespace {
std::string ToString(char32_t c) {
  std::string result;
  EncodeUTF8(c, result);
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

std::pair<CodepointIterator, CodepointIterator> BuildIterators(const std::string_view view) {
  const char* first = view.data();
  const char* last = first + view.size();
  return {CodepointIterator(first, first, last), CodepointIterator(last, first, last)};
}

std::pair<CodepointIterator, CodepointIterator> BuildIterators(const CodepointIterator begin,
                                                               const CodepointIterator end) {
  return {CodepointIterator(begin.Base(), begin.Base(), end.Base()),
          CodepointIterator(end.Base(), begin.Base(), end.Base())};
}

size_t ByteLength(CodepointIterator begin, CodepointIterator end) {
  return static_cast<size_t>(std::distance(begin.Base(), end.Base()));
}
}  // namespace

size_t utf8_count(const std::string_view view) {
  auto [begin, end] = BuildIterators(view);
  return static_cast<size_t>(std::distance(begin, end));
}

std::string utf8_lower_ascii(const std::string_view view) {
  auto [begin, end] = BuildIterators(view);
  std::string result;
  for (auto it = begin; it != end; ++it) {
    EncodeUTF8(static_cast<char32_t>(LowerASCII(*it)), result);
  }
  return result;
}

std::string utf8_upper_ascii(const std::string_view view) {
  auto [begin, end] = BuildIterators(view);
  std::string result;
  for (auto it = begin; it != end; ++it) {
    EncodeUTF8(static_cast<char32_t>(UpperASCII(*it)), result);
  }
  return result;
}

namespace {
template <bool LOWER>
inline size_t HashFNV1a(const std::string_view view) {
  const size_t offset_basis = 0xCBF29CE484222325ull;
  const size_t prime = 0x00000100000001B3ull;
  auto work = [](size_t hash, u8 byte_of_data) {
    hash ^= byte_of_data;
    hash *= prime;
    return hash;
  };
  auto hash = offset_basis;
  auto [begin, end] = BuildIterators(view);
  for (auto it = begin; it != end; ++it) {
    u32 c;
    if constexpr (LOWER) {
      c = LowerASCII(*it);
    } else {
      c = *it;
    }
    hash = work(hash, static_cast<u8>((c >> 0) & 0xFF));
    hash = work(hash, static_cast<u8>((c >> 8) & 0xFF));
    hash = work(hash, static_cast<u8>((c >> 16) & 0xFF));
    hash = work(hash, static_cast<u8>((c >> 24) & 0xFF));
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
inline CodepointIterator FindNeedle(CodepointIterator haystack_it, CodepointIterator haystack_end,
                                    CodepointIterator needle_begin, CodepointIterator needle_end) {
  return std::find_if(haystack_it, haystack_end, [&](const auto& c) {
    for (auto needle = needle_begin; needle != needle_end; ++needle) {
      if (c == *needle) {
        return true;
      }
    }
    return false;
  });
}

inline CodepointIterator FindNeedleCase(CodepointIterator haystack_it,
                                        CodepointIterator haystack_end,
                                        CodepointIterator needle_begin,
                                        CodepointIterator needle_end) {
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

  auto [haystack_begin, haystack_end] = BuildIterators(haystack);
  auto [needle_begin, needle_end] = BuildIterators(needles);

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
  auto [left_begin, left_end] = BuildIterators(left);
  auto [right_begin, right_end] = BuildIterators(right);
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
  auto [left_begin, left_end] = BuildIterators(left);
  auto [right_begin, right_end] = BuildIterators(right);
  return std::equal(left_begin, left_end, right_begin, right_end, EqualASCIICase);
}

bool utf8_equal_case_z(const std::string_view left, const std::string_view right) {
  if (left.empty()) {
    return right.empty();
  } else if (right.empty()) {
    return false;
  }
  auto [left_begin, left_end] = BuildIterators(left);
  auto [right_begin, right_end] = BuildIterators(right);
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

  auto [haystack_begin, haystack_end] = BuildIterators(haystack);
  auto [needle_begin, needle_end] = BuildIterators(needles);

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

  auto [haystack_begin, haystack_end] = BuildIterators(haystack);
  auto [needle_begin, needle_end] = BuildIterators(needles);

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

  auto [haystack_begin, haystack_end] = BuildIterators(haystack);
  auto [needle_begin, needle_end] = BuildIterators(needle);
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

    auto [sub_start, sub_end] = BuildIterators(it, end);
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

  auto [haystack_begin, haystack_end] = BuildIterators(haystack);
  auto [needle_begin, needle_end] = BuildIterators(needle);
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

    auto [sub_start, sub_end] = BuildIterators(it, end);
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
  auto [it, end] = BuildIterators(haystack);
  return *it == static_cast<u32>(needle);
}
}  // namespace

bool utf8_starts_with(const std::string_view haystack, const std::string_view needle) {
  if (needle.empty()) {
    return true;
  } else if (haystack.empty()) {
    return false;
  }

  auto [haystack_begin, haystack_end] = BuildIterators(haystack);
  auto [needle_begin, needle_end] = BuildIterators(needle);
  auto needle_count = utf8_count(needle);

  auto it = haystack_begin;
  auto end = it;
  for (size_t i = 0; i < needle_count; ++i) {
    if (end == haystack_end) {
      return false;
    }
    ++end;
  }

  auto [sub_start, sub_end] = BuildIterators(it, end);
  return std::equal(needle_begin, needle_end, sub_start, sub_end);
}

bool utf8_starts_with_case(const std::string_view haystack, const std::string_view needle) {
  if (needle.empty()) {
    return true;
  } else if (haystack.empty()) {
    return false;
  }

  auto [haystack_begin, haystack_end] = BuildIterators(haystack);
  auto [needle_begin, needle_end] = BuildIterators(needle);
  auto needle_count = utf8_count(needle);

  auto it = haystack_begin;
  auto end = it;
  for (size_t i = 0; i < needle_count; ++i) {
    if (end == haystack_end) {
      return false;
    }
    ++end;
  }

  auto [sub_start, sub_end] = BuildIterators(it, end);
  return std::equal(needle_begin, needle_end, sub_start, sub_end, EqualASCIICase);
}

bool utf8_ends_with(const std::string_view haystack, const std::string_view needle) {
  if (needle.empty()) {
    return true;
  } else if (haystack.empty()) {
    return false;
  }

  auto [haystack_begin, haystack_end] = BuildIterators(haystack);
  auto [needle_begin, needle_end] = BuildIterators(needle);
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

  auto [sub_start, sub_end] = BuildIterators(it, end);
  return std::equal(needle_begin, needle_end, sub_start, sub_end);
}

bool utf8_ends_with_case(const std::string_view haystack, const std::string_view needle) {
  if (needle.empty()) {
    return true;
  } else if (haystack.empty()) {
    return false;
  }

  auto [haystack_begin, haystack_end] = BuildIterators(haystack);
  auto [needle_begin, needle_end] = BuildIterators(needle);
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

  auto [sub_start, sub_end] = BuildIterators(it, end);
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

  CodepointIterator it;
  std::tie(std::ignore, it) = BuildIterators(left_path);
  --it;

  std::string result = std::string(left_path);
  if (*it != static_cast<u32>(separator)) {
    EncodeUTF8(separator, result);
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

  auto [path_begin, path_end] = BuildIterators(path);

  std::string result;
  auto it = path_begin;
  auto last = it;

  auto is_separator = [old_separator, new_separator](char32_t c) {
    return c == static_cast<u32>(old_separator) || c == static_cast<u32>(new_separator);
  };

  if (is_separator(*it)) {
    EncodeUTF8(new_separator, result);
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
      EncodeUTF8(new_separator, result);
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

  auto [begin, end] = BuildIterators(path);

  auto it = end;
  --it;

  while (*it == static_cast<u32>(separator)) {
    if (it == begin) {
      return std::string();
    }
    --it;
  }

  end = std::next(it);

  while (*it != static_cast<u32>(separator)) {
    if (it == begin) {
      break;
    }
    --it;
  }

  if (*it == static_cast<u32>(separator)) {
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

  auto [begin, end] = BuildIterators(name);

  auto it = end;
  --it;

  while (it != begin) {
    if (*it == static_cast<u32>('.')) {
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

  auto [begin, end] = BuildIterators(path);

  auto it = end;
  --it;

  while (*it == static_cast<u32>(separator)) {
    if (it == begin) {
      return std::string();
    }
    --it;
  }

  while (*it != static_cast<u32>(separator)) {
    if (it == begin) {
      return std::string();
    }
    --it;
  }

  auto sep_pos = it;

  while (*it == static_cast<u32>(separator)) {
    if (it == begin) {
      return std::string();
    }
    --it;
  }

  // A volume root such as "D:" keeps its separator.
  if (*it == static_cast<u32>(':')) {
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

std::string_view utf8_strip_leading_slashes(const std::string_view path) {
  return path.substr(std::min(path.find_first_not_of('/'), path.size()));
}

std::string NormalizeGuestPath(std::string_view path) {
  std::string result = rex::utf8_fix_path_separators(path, U'/');

  auto colon = result.find(':');
  if (colon != std::string::npos && colon + 1 < result.size() && result[colon + 1] == '/') {
    result.erase(0, colon + 2);
  }

  result = rex::utf8_canonicalize_path(result, U'/');

  result.erase(0, result.size() - utf8_strip_leading_slashes(result).size());

  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  return result;
}

}  // namespace rex
