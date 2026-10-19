/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cctype>
#include <charconv>
#include <string>
#include <string_view>

#include <rex/string/utf8.h>
#include <rex/cvar/registry.h>
#include <rex/types.h>

namespace rex::ui {

namespace detail {

inline bool TryParsePositiveInt32(std::string_view value, i32& value_out) {
  if (value.empty()) {
    return false;
  }

  i32 parsed_value = 0;
  auto [parse_end, parse_error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed_value);
  if (parse_error != std::errc() || parse_end != value.data() + value.size() || parsed_value <= 0) {
    return false;
  }

  value_out = parsed_value;
  return true;
}

}  // namespace detail

/**
 * Parses a resolution preset. Spaces, underscores and hyphens are ignored and case does not
 * matter.
 *
 * @param resolution_value Text such as "1920x1080", "1080p" or "4k".
 * @param width_out Receives the width.
 * @param height_out Receives the height.
 * @return True when the text names a size.
 */
inline bool TryParseResolutionPreset(std::string_view resolution_value, i32& width_out,
                                     i32& height_out) {
  std::string normalized;
  normalized.reserve(resolution_value.size());
  for (char c : resolution_value) {
    unsigned char c_unsigned = static_cast<unsigned char>(c);
    if (std::isspace(c_unsigned) || c == '_' || c == '-') {
      continue;
    }
    normalized.push_back(c);
  }
  normalized = utf8_lower_ascii(normalized);
  if (normalized.empty()) {
    return false;
  }

  size_t x_position = normalized.find('x');
  if (x_position != std::string::npos && x_position > 0 && x_position + 1 < normalized.size()) {
    i32 parsed_width = 0;
    i32 parsed_height = 0;
    if (!detail::TryParsePositiveInt32(std::string_view(normalized).substr(0, x_position),
                                       parsed_width) ||
        !detail::TryParsePositiveInt32(std::string_view(normalized).substr(x_position + 1),
                                       parsed_height)) {
      return false;
    }
    width_out = parsed_width;
    height_out = parsed_height;
    return true;
  }

  if (normalized == "480p") {
    width_out = 640;
    height_out = 480;
    return true;
  }
  if (normalized == "540p") {
    width_out = 960;
    height_out = 540;
    return true;
  }
  if (normalized == "720p") {
    width_out = 1280;
    height_out = 720;
    return true;
  }
  if (normalized == "900p") {
    width_out = 1600;
    height_out = 900;
    return true;
  }
  if (normalized == "1080p") {
    width_out = 1920;
    height_out = 1080;
    return true;
  }
  if (normalized == "1440p") {
    width_out = 2560;
    height_out = 1440;
    return true;
  }
  if (normalized == "1800p") {
    width_out = 3200;
    height_out = 1800;
    return true;
  }
  if (normalized == "2160p" || normalized == "4k") {
    width_out = 3840;
    height_out = 2160;
    return true;
  }
  return false;
}

/**
 * Reads ui.resolution and parses it as a preset.
 *
 * @param cvars Registry holding ui.resolution.
 * @param width_out Receives the width.
 * @param height_out Receives the height.
 * @return True when ui.resolution exists and names a size.
 */
inline bool TryGetResolutionPresetFromCvar(const cvar::ICvarRegistry& cvars, i32& width_out,
                                           i32& height_out) {
  auto value = cvars.Get("ui.resolution");
  return value && TryParseResolutionPreset(*value, width_out, height_out);
}

}  // namespace rex::ui
