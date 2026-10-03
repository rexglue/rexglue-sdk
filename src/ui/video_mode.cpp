#include <rex/ui/video_mode.h>

#include <algorithm>

#include "ui/cvar_table.h"

namespace rex::ui {

bool TryGetResolutionPresetFromCVar(i32& width_out, i32& height_out) {
  return TryParseResolutionPreset(REXCVAR_GET(::rex::ui::cvars::resolution), width_out, height_out);
}

bool ResolveConfiguredSize(i32& width_out, i32& height_out) {
  if (TryGetResolutionPresetFromCVar(width_out, height_out)) {
    return true;
  }
  i32 window_width = REXCVAR_GET(::rex::ui::cvars::window_width);
  i32 window_height = REXCVAR_GET(::rex::ui::cvars::window_height);
  if (window_width > 0 && window_height > 0) {
    width_out = window_width;
    height_out = window_height;
    return true;
  }
  width_out = REXCVAR_GET(::rex::ui::cvars::video_mode_width);
  height_out = REXCVAR_GET(::rex::ui::cvars::video_mode_height);
  return width_out > 0 && height_out > 0;
}

void ConfiguredVideoMode(u32& width, u32& height) {
  i32 configured_width = REXCVAR_GET(cvars::video_mode_width);
  i32 configured_height = REXCVAR_GET(cvars::video_mode_height);
  if (!rex::cvar::IsSet(cvars::video_mode_width) && !rex::cvar::IsSet(cvars::video_mode_height)) {
    ResolveConfiguredSize(configured_width, configured_height);
  }
  width = static_cast<u32>(std::clamp(configured_width, 640, 0x0FFF));
  height = static_cast<u32>(std::clamp(configured_height, 480, 0x0FFF));
}

f32 ConfiguredRefreshRate() {
  return static_cast<f32>(std::clamp(REXCVAR_GET(cvars::video_mode_refresh_rate), 24.0, 240.0));
}

}  // namespace rex::ui
