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

#include <rex/string/text.h>

#include "base/utf8_codec.h"

namespace rex::string {

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

#if !REX_PLATFORM_WIN32
std::wstring ToWideString(const std::string_view source) {
  std::wstring result;
  result.reserve(source.size());
  const char* it = source.data();
  const char* end = it + source.size();
  while (it != end) {
    const char32_t code_point = DecodeUTF8(it, end);
    result.push_back(static_cast<wchar_t>(IsInvalidByte(code_point) ? kReplacement : code_point));
  }
  return result;
}

std::string ToUTF8(const std::wstring_view source) {
  std::string result;
  result.reserve(source.size());
  for (const wchar_t unit : source) {
    const auto code_point = static_cast<char32_t>(unit);
    EncodeUTF8(IsInvalidByte(code_point) ? kReplacement : code_point, result);
  }
  return result;
}
#endif

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
u32 LowerASCII(const u32 c) {
  return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

u32 UpperASCII(const u32 c) {
  return c >= 'a' && c <= 'z' ? c - 32 : c;
}

bool EqualASCIICase(const u32 l, const u32 r) {
  return l == r || LowerASCII(l) == LowerASCII(r);
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
inline CodePointIterator FindNeedle(CodePointIterator haystack_it, CodePointIterator haystack_end,
                                    CodePointIterator needle_begin, CodePointIterator needle_end) {
  return std::find_if(haystack_it, haystack_end, [&](const auto& c) {
    for (auto needle = needle_begin; needle != needle_end; ++needle) {
      if (c == *needle) {
        return true;
      }
    }
    return false;
  });
}

inline CodePointIterator FindNeedleCase(CodePointIterator haystack_it,
                                        CodePointIterator haystack_end,
                                        CodePointIterator needle_begin,
                                        CodePointIterator needle_end) {
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
    return static_cast<std::string_view::size_type>(0);
  } else if (haystack.empty()) {
    return std::string_view::npos;
  }

  auto [haystack_begin, haystack_end] = BuildIterators(haystack);
  auto [needle_begin, needle_end] = BuildIterators(needles);

  auto it = FindNeedle(haystack_begin, haystack_end, needle_begin, needle_end);
  if (it == haystack_end) {
    return std::string_view::npos;
  }

  return static_cast<std::string_view::size_type>(ByteLength(haystack_begin, it));
}

std::string_view::size_type utf8_find_any_of_case(const std::string_view haystack,
                                                  const std::string_view needles) {
  if (needles.empty()) {
    return static_cast<std::string_view::size_type>(0);
  } else if (haystack.empty()) {
    return std::string_view::npos;
  }

  auto [haystack_begin, haystack_end] = BuildIterators(haystack);
  auto [needle_begin, needle_end] = BuildIterators(needles);

  auto it = FindNeedleCase(haystack_begin, haystack_end, needle_begin, needle_end);
  if (it == haystack_end) {
    return std::string_view::npos;
  }

  return static_cast<std::string_view::size_type>(ByteLength(haystack_begin, it));
}

std::string_view::size_type utf8_find_first_of(const std::string_view haystack,
                                               const std::string_view needle) {
  if (needle.empty()) {
    return static_cast<std::string_view::size_type>(0);
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
      return static_cast<std::string_view::size_type>(ByteLength(haystack_begin, it));
    }
  }

  return std::string_view::npos;
}

std::string_view::size_type utf8_find_first_of_case(const std::string_view haystack,
                                                    const std::string_view needle) {
  if (needle.empty()) {
    return static_cast<std::string_view::size_type>(0);
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
      return static_cast<std::string_view::size_type>(ByteLength(haystack_begin, it));
    }
  }

  return std::string_view::npos;
}

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

}  // namespace rex::string
