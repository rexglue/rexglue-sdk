/**
 * @file        ui/video_mode_util.cpp
 * @brief       Resolution preset parsing and the configured video mode
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/video_mode_util.h>

#include <algorithm>

#include "ui/cvar_table.h"

namespace rex::graphics::video_mode_util {

bool TryGetResolutionPresetFromCVar(int32_t& width_out, int32_t& height_out) {
  return TryParseResolutionPreset(REXCVAR_GET(::rex::ui::cvars::resolution), width_out, height_out);
}

bool ResolveConfiguredSize(int32_t& width_out, int32_t& height_out) {
  if (TryGetResolutionPresetFromCVar(width_out, height_out)) {
    return true;
  }
  int32_t window_width = REXCVAR_GET(::rex::ui::cvars::window_width);
  int32_t window_height = REXCVAR_GET(::rex::ui::cvars::window_height);
  if (window_width > 0 && window_height > 0) {
    width_out = window_width;
    height_out = window_height;
    return true;
  }
  width_out = REXCVAR_GET(::rex::ui::cvars::video_mode_width);
  height_out = REXCVAR_GET(::rex::ui::cvars::video_mode_height);
  return width_out > 0 && height_out > 0;
}

}  // namespace rex::graphics::video_mode_util

namespace rex::ui {

void ConfiguredVideoMode(uint32_t& width, uint32_t& height) {
  int32_t configured_width = REXCVAR_GET(cvars::video_mode_width);
  int32_t configured_height = REXCVAR_GET(cvars::video_mode_height);
  if (!rex::cvar::IsSet(cvars::video_mode_width) && !rex::cvar::IsSet(cvars::video_mode_height)) {
    graphics::video_mode_util::ResolveConfiguredSize(configured_width, configured_height);
  }
  width = uint32_t(std::clamp(configured_width, 640, 0x0FFF));
  height = uint32_t(std::clamp(configured_height, 480, 0x0FFF));
}

float ConfiguredRefreshRate() {
  return float(std::clamp(REXCVAR_GET(cvars::video_mode_refresh_rate), 24.0, 240.0));
}

}  // namespace rex::ui
