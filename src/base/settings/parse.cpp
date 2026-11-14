/**
 * @file        base/settings/parse.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <charconv>
#include <cstdlib>
#include <format>

#include <rex/settings_registry.h>

namespace rex {

namespace {

template <typename T>
bool ParseInteger(std::string_view text, T& out) {
  T value{};
  auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc() || end != text.data() + text.size()) {
    return false;
  }
  out = value;
  return true;
}

}  // namespace

namespace detail {

bool ParseSetting(std::string_view text, bool& out) {
  if (text == "true" || text == "1") {
    out = true;
    return true;
  }
  if (text == "false" || text == "0") {
    out = false;
    return true;
  }
  return false;
}

bool ParseSetting(std::string_view text, i32& out) {
  return ParseInteger(text, out);
}

bool ParseSetting(std::string_view text, i64& out) {
  return ParseInteger(text, out);
}

bool ParseSetting(std::string_view text, u32& out) {
  return ParseInteger(text, out);
}

bool ParseSetting(std::string_view text, u64& out) {
  return ParseInteger(text, out);
}

bool ParseSetting(std::string_view text, f64& out) {
  const std::string copy(text);
  char* end = nullptr;
  const f64 value = std::strtod(copy.c_str(), &end);
  if (copy.empty() || end != copy.c_str() + copy.size()) {
    return false;
  }
  out = value;
  return true;
}

bool ParseSetting(std::string_view text, std::string& out) {
  out = std::string(text);
  return true;
}

bool ParseSetting(std::string_view text, std::filesystem::path& out) {
  out = std::filesystem::path(std::string(text));
  return true;
}

std::string RenderSetting(bool value) {
  return value ? "true" : "false";
}

std::string RenderSetting(i32 value) {
  return std::to_string(value);
}

std::string RenderSetting(i64 value) {
  return std::to_string(value);
}

std::string RenderSetting(u32 value) {
  return std::to_string(value);
}

std::string RenderSetting(u64 value) {
  return std::to_string(value);
}

std::string RenderSetting(f64 value) {
  return std::format("{}", value);
}

std::string RenderSetting(const std::string& value) {
  return value;
}

std::string RenderSetting(const std::filesystem::path& value) {
  return value.generic_string();
}

std::filesystem::path ResolveSettingPath(const std::filesystem::path& value) {
  std::string text = value.generic_string();
  SettingsRegistry& settings = REX_SETTINGS();
  for (int pass = 0; pass < 8; ++pass) {
    const size_t open = text.find('{');
    if (open == std::string::npos)
      break;
    const size_t close = text.find('}', open);
    if (close == std::string::npos)
      break;
    const std::string name = text.substr(open + 1, close - open - 1);
    auto found = settings.Find(name);
    if (!found)
      break;
    text.replace(open, close - open + 1, (*found)->GetText());
  }
  return std::filesystem::path(text);
}

}  // namespace detail

}  // namespace rex
