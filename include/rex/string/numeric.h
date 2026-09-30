/**
 * @file        rex/string/numeric.h
 * @brief       Hex formatters and string-to-numeric parsers.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
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
#include <rex/vec128.h>

#if REX_PLATFORM_MAC
#include <locale.h>
#endif

namespace rex {

inline std::string to_hex_string(uint32_t value) {
  return fmt::format("{:08X}", value);
}

inline std::string to_hex_string(uint64_t value) {
  return fmt::format("{:016X}", value);
}

inline std::string to_hex_string(float value) {
  static_assert(sizeof(uint32_t) == sizeof(value));
  uint32_t pun;
  std::memcpy(&pun, &value, sizeof(value));
  return to_hex_string(pun);
}

inline std::string to_hex_string(double value) {
  static_assert(sizeof(uint64_t) == sizeof(value));
  uint64_t pun;
  std::memcpy(&pun, &value, sizeof(value));
  return to_hex_string(pun);
}

inline std::string to_hex_string(const vec128_t& value) {
  return fmt::format("[{:08X} {:08X} {:08X} {:08X}]", value.u32[0], value.u32[1], value.u32[2],
                     value.u32[3]);
}

template <typename T>
inline T from_string(const std::string_view value, bool force_hex = false) {
  (void)value;
  (void)force_hex;
  // Missing implementation for converting type T from string
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

// integral_from_string
template <typename T>
inline T ifs(const std::string_view value, bool force_hex) {
  int base = 10;
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
  // TODO(gibbed): do something more with errors?
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
  static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>);

  // AppleClang's libc++ does not provide floating-point std::from_chars on all
  // supported SDK versions. Match from_chars semantics with a C-locale parser:
  // no leading whitespace or '+', no global locale dependency, and distinct
  // invalid-input and range errors.
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
  if constexpr (std::is_same_v<T, float>) {
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

// floating_point_from_string
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
    // TODO(gibbed): do something more with errors?
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

// The single definition of what counts as a true boolean string. Config files,
// the command line, cvar change callbacks and the settings UI all funnel
// through here so a value accepted in one place can't be rejected in another.
template <>
inline bool from_string<bool>(const std::string_view value, bool force_hex) {
  (void)force_hex;
  return value == "true" || value == "1" || value == "yes";
}

template <>
inline int8_t from_string<int8_t>(const std::string_view value, bool force_hex) {
  return detail::ifs<int8_t>(value, force_hex);
}

template <>
inline uint8_t from_string<uint8_t>(const std::string_view value, bool force_hex) {
  return detail::ifs<uint8_t>(value, force_hex);
}

template <>
inline int16_t from_string<int16_t>(const std::string_view value, bool force_hex) {
  return detail::ifs<int16_t>(value, force_hex);
}

template <>
inline uint16_t from_string<uint16_t>(const std::string_view value, bool force_hex) {
  return detail::ifs<uint16_t>(value, force_hex);
}

template <>
inline int32_t from_string<int32_t>(const std::string_view value, bool force_hex) {
  return detail::ifs<int32_t>(value, force_hex);
}

template <>
inline uint32_t from_string<uint32_t>(const std::string_view value, bool force_hex) {
  return detail::ifs<uint32_t>(value, force_hex);
}

template <>
inline int64_t from_string<int64_t>(const std::string_view value, bool force_hex) {
  return detail::ifs<int64_t>(value, force_hex);
}

template <>
inline uint64_t from_string<uint64_t>(const std::string_view value, bool force_hex) {
  return detail::ifs<uint64_t>(value, force_hex);
}

template <>
inline float from_string<float>(const std::string_view value, bool force_hex) {
  return detail::fpfs<float, uint32_t>(value, force_hex);
}

template <>
inline double from_string<double>(const std::string_view value, bool force_hex) {
  return detail::fpfs<double, uint64_t>(value, force_hex);
}

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
    // Assume hex?
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
