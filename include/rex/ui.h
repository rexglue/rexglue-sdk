/**
 * @file        ui.h
 * @brief       UI module facade and configuration slice
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <string>

namespace rex::ui {

/** Initial window placement. Supplies the ui.window_* and ui.fullscreen* cvar defaults. */
struct WindowConfig {
  /** Logical width. 0 means resolve from ui.resolution and ui.video_mode_width. */
  int32_t width = 0;
  /** Logical height. 0 means resolve from ui.resolution and ui.video_mode_height. */
  int32_t height = 0;
  /** Start fullscreen. */
  bool fullscreen = true;
  /** Exclusive fullscreen where the platform has it. */
  bool fullscreen_exclusive = false;
  /** Monitor index. */
  int32_t monitor = 0;
};

/** UI module slice. */
struct UIConfig {
  /** Initial window placement. */
  WindowConfig window;
  /** SDL_HINT_VIDEO_DRIVER. Empty means SDL's default, "cocoa" on macOS. */
  std::string video_driver;
};

}  // namespace rex::ui
