/**
 * @file        string/numeric.h
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

#include <bit>
#include <charconv>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>

#include <rex/assert.h>
#include <rex/platform.h>
#include <rex/string/text.h>
#include <rex/types.h>

#if REX_PLATFORM_MAC
#include <locale.h>
#endif

namespace rex::string {

/**
 * Formats a 32 bit value as 8 uppercase hex digits.
 *
 * @return The hex digits without a prefix.
 */
inline std::string to_hex_string(u32 value) {
  return std::format("{:08X}", value);
}

/**
 * Formats a 64 bit value as 16 uppercase hex digits.
 *
 * @return The hex digits without a prefix.
 */
inline std::string to_hex_string(u64 value) {
  return std::format("{:016X}", value);
}

/**
 * Formats the bits of a 32 bit float as 8 uppercase hex digits.
 *
 * @return The hex digits without a prefix.
 */
inline std::string to_hex_string(f32 value) {
  static_assert(sizeof(u32) == sizeof(value));
  u32 pun = std::bit_cast<u32>(value);
  return to_hex_string(pun);
}

/**
 * Formats the bits of a 64 bit float as 16 uppercase hex digits.
 *
 * @return The hex digits without a prefix.
 */
inline std::string to_hex_string(f64 value) {
  static_assert(sizeof(u64) == sizeof(value));
  u64 pun = std::bit_cast<u64>(value);
  return to_hex_string(pun);
}

/**
 * Parses a value from a string. Only the specializations parse.
 *
 * @param force_hex True to parse integers and floats as hex without a prefix.
 */
template <typename T>
inline T from_string(const std::string_view value, bool force_hex = false) {
  (void)value;
  (void)force_hex;
  throw;
}

namespace detail {

template <typename T, typename V = std::make_signed_t<T>>
inline T MakeNegative(T value) {
  if constexpr (std::is_unsigned_v<T>) {
    value = static_cast<T>(-static_cast<V>(value));
  } else {
    value = -value;
  }
  return value;
}

template <typename T>
inline T ParseInteger(const std::string_view value, bool force_hex) {
  i32 base = 10;
  std::string_view range = value;
  bool is_hex = force_hex;
  bool is_negative = false;
  if (rex::string::utf8_starts_with(range, "-")) {
    is_negative = true;
    range = range.substr(1);
  }
  if (rex::string::utf8_starts_with(range, "0x")) {
    is_hex = true;
    range = range.substr(2);
  }
  if (rex::string::utf8_ends_with(range, "h")) {
    is_hex = true;
    range = range.substr(0, range.length() - 1);
  }
  T result;
  if (is_hex) {
    base = 16;
  }
  auto [p, error] = std::from_chars(range.data(), range.data() + range.size(), result, base);
  if (error != std::errc()) {
    REX_ASSERT_ALWAYS();
    return T();
  }
  if (is_negative) {
    result = MakeNegative(result);
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
inline T ParseFloat(const std::string_view value, bool force_hex) {
  static_assert(sizeof(T) == sizeof(PUN));
  std::string_view range = value;
  bool is_hex = force_hex;
  bool is_negative = false;
  if (rex::string::utf8_starts_with(range, "-")) {
    is_negative = true;
    range = range.substr(1);
  }
  if (rex::string::utf8_starts_with(range, "0x")) {
    is_hex = true;
    range = range.substr(2);
  }
  if (rex::string::utf8_ends_with(range, "h")) {
    is_hex = true;
    range = range.substr(0, range.length() - 1);
  }
  T result;
  if (is_hex) {
    PUN pun = from_string<PUN>(range, true);
    if (is_negative) {
      pun = MakeNegative(pun);
    }
    result = std::bit_cast<T>(pun);
  } else {
#if REX_PLATFORM_MAC
    auto [p, error] = portable_float_from_chars(range.data(), range.data() + range.size(), result);
#else
    auto [p, error] = std::from_chars(range.data(), range.data() + range.size(), result,
                                      std::chars_format::general);
#endif
    if (error != std::errc()) {
      REX_ASSERT_ALWAYS();
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
 * Parses a boolean. Accepts "true", "1" or "yes".
 *
 * @param force_hex Unused.
 * @return True for "true", "1" or "yes", false otherwise.
 */
template <>
inline bool from_string<bool>(const std::string_view value, bool force_hex) {
  (void)force_hex;
  return value == "true" || value == "1" || value == "yes";
}

/**
 * Parses a signed 8 bit integer. Hex takes a "0x" prefix or an "h" suffix.
 *
 * @param force_hex True to parse as hex without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline i8 from_string<i8>(const std::string_view value, bool force_hex) {
  return detail::ParseInteger<i8>(value, force_hex);
}

/**
 * Parses an unsigned 8 bit integer. Hex takes a "0x" prefix or an "h" suffix.
 *
 * @param force_hex True to parse as hex without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline u8 from_string<u8>(const std::string_view value, bool force_hex) {
  return detail::ParseInteger<u8>(value, force_hex);
}

/**
 * Parses a signed 16 bit integer. Hex takes a "0x" prefix or an "h" suffix.
 *
 * @param force_hex True to parse as hex without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline i16 from_string<i16>(const std::string_view value, bool force_hex) {
  return detail::ParseInteger<i16>(value, force_hex);
}

/**
 * Parses an unsigned 16 bit integer. Hex takes a "0x" prefix or an "h" suffix.
 *
 * @param force_hex True to parse as hex without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline u16 from_string<u16>(const std::string_view value, bool force_hex) {
  return detail::ParseInteger<u16>(value, force_hex);
}

/**
 * Parses a signed 32 bit integer. Hex takes a "0x" prefix or an "h" suffix.
 *
 * @param force_hex True to parse as hex without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline i32 from_string<i32>(const std::string_view value, bool force_hex) {
  return detail::ParseInteger<i32>(value, force_hex);
}

/**
 * Parses an unsigned 32 bit integer. Hex takes a "0x" prefix or an "h" suffix.
 *
 * @param force_hex True to parse as hex without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline u32 from_string<u32>(const std::string_view value, bool force_hex) {
  return detail::ParseInteger<u32>(value, force_hex);
}

/**
 * Parses a signed 64 bit integer. Hex takes a "0x" prefix or an "h" suffix.
 *
 * @param force_hex True to parse as hex without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline i64 from_string<i64>(const std::string_view value, bool force_hex) {
  return detail::ParseInteger<i64>(value, force_hex);
}

/**
 * Parses an unsigned 64 bit integer. Hex takes a "0x" prefix or an "h" suffix.
 *
 * @param force_hex True to parse as hex without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline u64 from_string<u64>(const std::string_view value, bool force_hex) {
  return detail::ParseInteger<u64>(value, force_hex);
}

/**
 * Parses a 32 bit float. Hex text gives the raw bits.
 *
 * @param force_hex True to parse as raw hex bits without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline f32 from_string<f32>(const std::string_view value, bool force_hex) {
  return detail::ParseFloat<f32, u32>(value, force_hex);
}

/**
 * Parses a 64 bit float. Hex text gives the raw bits.
 *
 * @param force_hex True to parse as raw hex bits without a prefix.
 * @return The value, or 0 when the text does not parse.
 */
template <>
inline f64 from_string<f64>(const std::string_view value, bool force_hex) {
  return detail::ParseFloat<f64, u64>(value, force_hex);
}

}  // namespace rex::string
