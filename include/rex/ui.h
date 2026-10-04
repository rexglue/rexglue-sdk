/**
 * @file        ui.h
 * @brief       UI module facade and module config
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>

#include <rex/types.h>

namespace rex::ui {

/** Initial window placement. Supplies the ui.window_* and ui.fullscreen* cvar defaults. */
struct WindowConfig {
  /** Logical width. 0 means resolve from ui.resolution and ui.video_mode_width. */
  i32 width = 0;
  /** Logical height. 0 means resolve from ui.resolution and ui.video_mode_height. */
  i32 height = 0;
  /** Start fullscreen. */
  bool fullscreen = true;
  /** Exclusive fullscreen where the platform has it. */
  bool fullscreen_exclusive = false;
  /** Monitor index. */
  i32 monitor = 0;
};

/** Client area shape a window holds while the user resizes it. */
enum class AspectRatio : u8 {
  kStretch, /**< Any shape. The image stretches to fill the client area. */
  k4x3,     /**< 4 wide by 3 high. */
  k5x4,     /**< 5 wide by 4 high. */
  k3x2,     /**< 3 wide by 2 high. */
  k16x10,   /**< 16 wide by 10 high. */
  k16x9,    /**< 16 wide by 9 high. */
  k21x9,    /**< 21 wide by 9 high, ultrawide. */
  k32x9,    /**< 32 wide by 9 high, super ultrawide. */
};

/** Size of a window's client area. */
struct WindowSize {
  /** Width in pixels. */
  u32 width_px = 0;
  /** Height in pixels. */
  u32 height_px = 0;
};

/** UI module config. */
struct UIConfig {
  /** Initial window placement. */
  WindowConfig window;
  /** SDL_HINT_VIDEO_DRIVER. Empty means SDL's default, "cocoa" on macOS. */
  std::string video_driver;
  /** Window title. Empty uses the executable name and the SDK build stamp. */
  std::string title;
  /** Smallest client area the window accepts. A 0 field means no limit on that axis. */
  WindowSize min_size;
  /** Client area shape the window holds while the user resizes it. */
  AspectRatio aspect_ratio = AspectRatio::kStretch;
};

}  // namespace rex::ui
