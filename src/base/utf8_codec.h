/**
 * @file        base/utf8_codec.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include <rex/types.h>

namespace rex::string {

inline constexpr char32_t kReplacement = 0xFFFD;
inline constexpr char32_t kInvalidByteBase = 0xDC00;
inline constexpr char32_t kInvalidByteFirst = kInvalidByteBase + 0x80;
inline constexpr char32_t kInvalidByteLast = kInvalidByteBase + 0xFF;

inline bool IsInvalidByte(char32_t code_point) {
  return code_point >= kInvalidByteFirst && code_point <= kInvalidByteLast;
}

inline bool IsContinuation(char c) {
  return (static_cast<u8>(c) & 0xC0) == 0x80;
}

inline bool IsEncodable(char32_t code_point) {
  return code_point <= 0x10FFFF && (code_point < 0xD800 || code_point > 0xDFFF);
}

inline char32_t DecodeUTF8(const char*& it, const char* end) {
  const char* const start = it;
  const auto lead = static_cast<u8>(*it++);
  if (lead < 0x80) {
    return lead;
  }
  const auto invalid = [&]() {
    it = start + 1;
    return kInvalidByteBase + lead;
  };
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
    return invalid();
  }
  for (u32 i = 0; i < trail_count; ++i) {
    if (it == end) {
      return invalid();
    }
    const auto trail = static_cast<u8>(*it);
    if (trail < low || trail > high) {
      return invalid();
    }
    code_point = (code_point << 6) | (trail & 0x3F);
    low = 0x80;
    high = 0xBF;
    ++it;
  }
  return code_point;
}

inline void EncodeUTF8(char32_t code_point, std::string& out) {
  if (IsInvalidByte(code_point)) {
    out.push_back(static_cast<char>(code_point - kInvalidByteBase));
    return;
  }
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

inline char32_t DecodeUTF16(const char16_t*& it, const char16_t* end) {
  const char16_t unit = *it++;
  if (unit < 0xD800 || unit > 0xDFFF || IsInvalidByte(unit)) {
    return unit;
  }
  if (unit > 0xDBFF || it == end || *it < 0xDC00 || *it > 0xDFFF) {
    return kReplacement;
  }
  const char16_t trail = *it++;
  return 0x10000 + ((static_cast<char32_t>(unit) - 0xD800) << 10) +
         (static_cast<char32_t>(trail) - 0xDC00);
}

inline void EncodeUTF16(char32_t code_point, std::u16string& out) {
  if (IsInvalidByte(code_point)) {
    out.push_back(static_cast<char16_t>(code_point));
    return;
  }
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

class CodePointIterator {
 public:
  using iterator_category = std::bidirectional_iterator_tag;
  using value_type = u32;
  using difference_type = std::ptrdiff_t;
  using pointer = void;
  using reference = u32;

  CodePointIterator() = default;
  CodePointIterator(const char* position, const char* range_begin, const char* range_end)
      : position_(position), range_begin_(range_begin), range_end_(range_end) {}

  [[nodiscard]] const char* Base() const { return position_; }

  u32 operator*() const {
    const char* it = position_;
    return DecodeUTF8(it, range_end_);
  }

  CodePointIterator& operator++() {
    DecodeUTF8(position_, range_end_);
    return *this;
  }

  CodePointIterator operator++(int) {
    CodePointIterator previous = *this;
    ++*this;
    return previous;
  }

  CodePointIterator& operator--() {
    const char* start = position_ - 1;
    for (u32 i = 0; i < 3 && start > range_begin_ && IsContinuation(*start); ++i) {
      --start;
    }
    const char* decoded_end = start;
    DecodeUTF8(decoded_end, range_end_);
    position_ = decoded_end == position_ ? start : position_ - 1;
    return *this;
  }

  CodePointIterator operator--(int) {
    CodePointIterator previous = *this;
    --*this;
    return previous;
  }

  bool operator==(const CodePointIterator& other) const { return position_ == other.position_; }

 private:
  const char* position_ = nullptr;
  const char* range_begin_ = nullptr;
  const char* range_end_ = nullptr;
};

inline std::string ToString(char32_t c) {
  std::string result;
  EncodeUTF8(c, result);
  return result;
}

inline std::pair<CodePointIterator, CodePointIterator> BuildIterators(const std::string_view view) {
  const char* first = view.data();
  const char* last = first + view.size();
  return {CodePointIterator(first, first, last), CodePointIterator(last, first, last)};
}

inline std::pair<CodePointIterator, CodePointIterator> BuildIterators(const CodePointIterator begin,
                                                                      const CodePointIterator end) {
  return {CodePointIterator(begin.Base(), begin.Base(), end.Base()),
          CodePointIterator(end.Base(), begin.Base(), end.Base())};
}

inline size_t ByteLength(CodePointIterator begin, CodePointIterator end) {
  return static_cast<size_t>(std::distance(begin.Base(), end.Base()));
}

inline bool StartsWithCodePoint(const std::string_view haystack, char32_t needle) {
  if (haystack.empty()) {
    return false;
  }
  auto [it, end] = BuildIterators(haystack);
  return *it == static_cast<u32>(needle);
}

}  // namespace rex::string
