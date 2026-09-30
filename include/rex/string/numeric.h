/**
 * @file        string/numeric.h
 * @brief       Hex formatters and string to number parsers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/string_util.h (Ben Vanik, 2020).
 */

#pragma once

#include <charconv>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

#include <fmt/format.h>

#include <rex/assert.h>
#include <rex/platform.h>
#include <rex/string/utf8.h>
#include <rex/types.h>
#include <rex/vec128.h>

#if REX_PLATFORM_MAC
#include <locale.h>
#endif

namespace rex {

/**
 * Formats a 32 bit value as 8 uppercase hex digits.
 *
 * @param value Value to format.
 * @return The hex digits without a prefix.
 */
inline std::string to_hex_string(u32 value) {
  return fmt::format("{:08X}", value);
}

/**
 * Formats a 64 bit value as 16 uppercase hex digits.
 *
 * @param value Value to format.
 * @return The hex digits without a prefix.
 */
inline std::string to_hex_string(u64 value) {
  return fmt::format("{:016X}", value);
}

/**
 * Formats the bits of a 32 bit float as 8 uppercase hex digits.
 *
 * @param value Value to format.
 * @return The hex digits without a prefix.
 */
inline std::string to_hex_string(f32 value) {
  static_assert(sizeof(u32) == sizeof(value));
  u32 pun;
  std::memcpy(&pun, &value, sizeof(value));
  return to_hex_string(pun);
}

/**
 * Formats the bits of a 64 bit float as 16 uppercase hex digits.
 *
 * @param value Value to format.
 * @return The hex digits without a prefix.
 */
inline std::string to_hex_string(f64 value) {
  static_assert(sizeof(u64) == sizeof(value));
  u64 pun;
  std::memcpy(&pun, &value, sizeof(value));
  return to_hex_string(pun);
}

/**
 * Formats the 4 32 bit lanes of a vector as hex.
 *
 * @param value Value to format.
 * @return The lanes as "[XXXXXXXX XXXXXXXX XXXXXXXX XXXXXXXX]".
 */
inline std::string to_hex_string(const vec128_t& value) {
  return fmt::format("[{:08X} {:08X} {:08X} {:08X}]", value.u32[0], value.u32[1], value.u32[2],
                     value.u32[3]);
}

/**
 * Parses a value from a string. Only the specializations in this header parse. The primary
 * template calls std::terminate.
 *
 * @tparam T Result type.
 * @param value Text to parse.
 * @param force_hex True to parse integers and floats as hex without a prefix.
 * @return The parsed value.
 */
template <typename T>
inline T from_string(const std::string_view value, bool force_hex = false) {
  (void)value;
  (void)force_hex;
  throw;
}

namespace detail {

template <typename T, typename V = std::make_signed_t<T>>
inline T make_negative(T value) {
  if constexpr (std::is_unsigned_v<T>) {
    value = static_cast<T>(-static_cast<V>(value));
  } else {
    value = -value;
  }
  return value;
}

template <typename T>
inline T ifs(const std::string_view value, bool force_hex) {
  i32 base = 10;
  std::string_view range = value;
  bool is_hex = force_hex;
  bool is_negative = false;
  if (rex::utf8_starts_with(range, "-")) {
    is_negative = true;
    range = range.substr(1);
  }
  if (rex::utf8_starts_with(range, "0x")) {
    is_hex = true;
    range = range.substr(2);
  }
  if (rex::utf8_ends_with(range, "h")) {
    is_hex = true;
    range = range.substr(0, range.length() - 1);
  }
  T result;
  if (is_hex) {
    base = 16;
  }
  auto [p, error] = std::from_chars(range.data(), range.data() + range.size(), result, base);
  if (error != std::errc()) {
    assert_always();
    return T();
  }
  if (is_negative) {
    result = make_negative(result);
  }
  return result;
}

#if REX_PLATFORM_MAC
template <typename T>
inline std::from_chars_result portable_float_from_chars(const char* first, const char* last,
                                                        T& value) {
  static_assert(std::is_same_v<T, f32> || std::is_same_v<T, f64>);

  if (first == last || *first == '+' || *first == ' ' || *first == '\f' || *first == '\n' ||
      *first == '\r' || *first == '\t' || *first == '\v') {
    return {first, std::errc::invalid_argument};
  }

  const size_t len = static_cast<size_t>(last - first);
  char buffer[128];
  if (len >= sizeof(buffer)) {
    return {first, std::errc::result_out_of_range};
  }
  std::memcpy(buffer, first, len);
  buffer[len] = '\0';

  static locale_t c_numeric_locale = newlocale(LC_NUMERIC_MASK, "C", nullptr);
  if (!c_numeric_locale) {
    return {first, std::errc::invalid_argument};
  }

  char* end = nullptr;
  errno = 0;
  T parsed_value;
  if constexpr (std::is_same_v<T, f32>) {
    parsed_value = strtof_l(buffer, &end, c_numeric_locale);
  } else {
    parsed_value = strtod_l(buffer, &end, c_numeric_locale);
  }
  if (end == buffer) {
    return {first, std::errc::invalid_argument};
  }
  if (errno == ERANGE) {
    return {first + (end - buffer), std::errc::result_out_of_range};
  }

  value = parsed_value;
  return {first + (end - buffer), std::errc()};
}
#endif

template <typename T, typename PUN>
inline T fpfs(const std::string_view value, bool force_hex) {
  static_assert(sizeof(T) == sizeof(PUN));
  std::string_view range = value;
  bool is_hex = force_hex;
  bool is_negative = false;
  if (rex::utf8_starts_with(range, "-")) {
    is_negative = true;
    range = range.substr(1);
  }
  if (rex::utf8_starts_with(range, "0x")) {
    is_hex = true;
    range = range.substr(2);
  }
  if (rex::utf8_ends_with(range, "h")) {
    is_hex = true;
    range = range.substr(0, range.length() - 1);
  }
  T result;
  if (is_hex) {
    PUN pun = from_string<PUN>(range, true);
    if (is_negative) {
      pun = make_negative(pun);
    }
    std::memcpy(&result, &pun, sizeof(PUN));
  } else {
#if REX_PLATFORM_MAC
    auto [p, error] = portable_float_from_chars(range.data(), range.data() + range.size(), result);
#else
    auto [p, error] = std::from_chars(range.data(), range.data() + range.size(), result,
                                      std::chars_format::general);
#endif
    if (error != std::errc()) {
      assert_always();
      return T();
    }
    if (is_negative) {
      result = -result;
    }
  }
  return result;
}

}  // namespace detail

/**
 * Parses a boolean. Config files, the command line, cvar callbacks and the settings UI all parse
 * booleans here, so every source accepts the same strings.
 *
 * @param value Text to parse.
 * @param force_hex Unused.
 * @return True for "true", "1" or "yes", false otherwise.
 */
template <>
inline bool from_string<bool>(const std::string_view value, bool force_hex) {
  (void)force_hex;
  return value == "true" || value == "1" || value == "yes";
}

/**
 * Parses a signed 8 bit integer. Accepts a leading "-", a "0x" prefix or an "h" suffix for hex.
 *
 * @param value Text to parse.
 * @param force_hex True to parse as hex without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline i8 from_string<i8>(const std::string_view value, bool force_hex) {
  return detail::ifs<i8>(value, force_hex);
}

/**
 * Parses an unsigned 8 bit integer. Accepts a leading "-", a "0x" prefix or an "h" suffix for
 * hex.
 *
 * @param value Text to parse.
 * @param force_hex True to parse as hex without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline u8 from_string<u8>(const std::string_view value, bool force_hex) {
  return detail::ifs<u8>(value, force_hex);
}

/**
 * Parses a signed 16 bit integer. Accepts a leading "-", a "0x" prefix or an "h" suffix for
 * hex.
 *
 * @param value Text to parse.
 * @param force_hex True to parse as hex without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline i16 from_string<i16>(const std::string_view value, bool force_hex) {
  return detail::ifs<i16>(value, force_hex);
}

/**
 * Parses an unsigned 16 bit integer. Accepts a leading "-", a "0x" prefix or an "h" suffix for
 * hex.
 *
 * @param value Text to parse.
 * @param force_hex True to parse as hex without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline u16 from_string<u16>(const std::string_view value, bool force_hex) {
  return detail::ifs<u16>(value, force_hex);
}

/**
 * Parses a signed 32 bit integer. Accepts a leading "-", a "0x" prefix or an "h" suffix for
 * hex.
 *
 * @param value Text to parse.
 * @param force_hex True to parse as hex without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline i32 from_string<i32>(const std::string_view value, bool force_hex) {
  return detail::ifs<i32>(value, force_hex);
}

/**
 * Parses an unsigned 32 bit integer. Accepts a leading "-", a "0x" prefix or an "h" suffix for
 * hex.
 *
 * @param value Text to parse.
 * @param force_hex True to parse as hex without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline u32 from_string<u32>(const std::string_view value, bool force_hex) {
  return detail::ifs<u32>(value, force_hex);
}

/**
 * Parses a signed 64 bit integer. Accepts a leading "-", a "0x" prefix or an "h" suffix for
 * hex.
 *
 * @param value Text to parse.
 * @param force_hex True to parse as hex without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline i64 from_string<i64>(const std::string_view value, bool force_hex) {
  return detail::ifs<i64>(value, force_hex);
}

/**
 * Parses an unsigned 64 bit integer. Accepts a leading "-", a "0x" prefix or an "h" suffix for
 * hex.
 *
 * @param value Text to parse.
 * @param force_hex True to parse as hex without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline u64 from_string<u64>(const std::string_view value, bool force_hex) {
  return detail::ifs<u64>(value, force_hex);
}

/**
 * Parses a 32 bit float. Hex text, marked by a "0x" prefix or an "h" suffix, gives the raw bits.
 *
 * @param value Text to parse.
 * @param force_hex True to parse as raw hex bits without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline f32 from_string<f32>(const std::string_view value, bool force_hex) {
  return detail::fpfs<f32, u32>(value, force_hex);
}

/**
 * Parses a 64 bit float. Hex text, marked by a "0x" prefix or an "h" suffix, gives the raw bits.
 *
 * @param value Text to parse.
 * @param force_hex True to parse as raw hex bits without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline f64 from_string<f64>(const std::string_view value, bool force_hex) {
  return detail::fpfs<f64, u64>(value, force_hex);
}

/**
 * Parses 4 lanes. "[a b c d]" or text without a bracket gives 4 hex 32 bit lanes. "(a, b, c, d)"
 * gives 4 floats. Lanes are separated by spaces or commas.
 *
 * @param value Text to parse.
 * @param force_hex Unused, the opening bracket picks the format.
 * @return The vector, or a zero vector when the text does not parse.
 */
template <>
inline vec128_t from_string<vec128_t>(const std::string_view value, bool force_hex) {
  if (!value.size()) {
    return vec128_t();
  }
  vec128_t v;
  auto p = value.data();
  auto end = value.data() + value.size();
  bool is_hex = force_hex;
  if (p != end && *p == '[') {
    is_hex = true;
    ++p;
  } else if (p != end && *p == '(') {
    is_hex = false;
    ++p;
  } else {
    is_hex = true;
  }
  if (p == end) {
    assert_always();
    return vec128_t();
  }
  if (is_hex) {
    for (size_t i = 0; i < 4; i++) {
      while (p != end && (*p == ' ' || *p == ',')) {
        ++p;
      }
      if (p == end) {
        assert_always();
        return vec128_t();
      }
      auto result = std::from_chars(p, end, v.u32[i], 16);
      if (result.ec != std::errc()) {
        assert_always();
        return vec128_t();
      }
      p = result.ptr;
    }
  } else {
    for (size_t i = 0; i < 4; i++) {
      while (p != end && (*p == ' ' || *p == ',')) {
        ++p;
      }
      if (p == end) {
        assert_always();
        return vec128_t();
      }
#if REX_PLATFORM_MAC
      auto result = detail::portable_float_from_chars(p, end, v.f32[i]);
#else
      auto result = std::from_chars(p, end, v.f32[i], std::chars_format::general);
#endif
      if (result.ec != std::errc()) {
        assert_always();
        return vec128_t();
      }
      p = result.ptr;
    }
  }
  return v;
}

}  // namespace rex
