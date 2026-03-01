/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <charconv>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>

namespace rex::graphics::video_mode_util {

inline bool TryParsePositiveInt32(std::string_view value, int32_t& value_out) {
  if (value.empty()) {
    return false;
  }

  int32_t parsed_value = 0;
  auto [parse_end, parse_error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed_value);
  if (parse_error != std::errc() || parse_end != value.data() + value.size() || parsed_value <= 0) {
    return false;
  }

  value_out = parsed_value;
  return true;
}

inline bool TryParseResolutionPreset(std::string_view resolution_value, int32_t& width_out,
                                     int32_t& height_out) {
  std::string normalized;
  normalized.reserve(resolution_value.size());
  for (char c : resolution_value) {
    unsigned char c_unsigned = static_cast<unsigned char>(c);
    if (std::isspace(c_unsigned) || c == '_' || c == '-') {
      continue;
    }
    normalized.push_back(char(std::tolower(c_unsigned)));
  }
  if (normalized.empty()) {
    return false;
  }

  size_t x_position = normalized.find('x');
  if (x_position != std::string::npos && x_position > 0 && x_position + 1 < normalized.size()) {
    int32_t parsed_width = 0;
    int32_t parsed_height = 0;
    if (!TryParsePositiveInt32(std::string_view(normalized).substr(0, x_position), parsed_width) ||
        !TryParsePositiveInt32(std::string_view(normalized).substr(x_position + 1),
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

inline bool TryGetResolutionPresetFromCVar(int32_t& width_out, int32_t& height_out) {
  if (!rex::cvar::HasNonDefaultValue("resolution")) {
    return false;
  }
  const std::string& resolution_value = REXCVAR_GET(resolution);
  if (resolution_value.empty()) {
    return false;
  }
  return TryParseResolutionPreset(resolution_value, width_out, height_out);
}

}  // namespace rex::graphics::video_mode_util
