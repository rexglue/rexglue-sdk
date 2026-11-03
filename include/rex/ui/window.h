/**
 * @file        ui/window.h
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

/** Client area shape a window holds while the user resizes it. */
enum class AspectRatio : u8 {
  kStretch, /**< Any shape. The image stretches to fill the client area. */
  k4x3,
  k5x4,
  k3x2,
  k16x10,
  k16x9,
  k21x9,
  k32x9,
};

struct WindowSize {
  u32 width_px = 0;
  u32 height_px = 0;
};

enum class CursorVisibility : u8 {
  kVisible,
  kAutoHidden, /**< Shown while the mouse moves, hidden after the auto hide delay. */
  kHidden,
};

struct Monitor {
  /** Index to pass to IWindow::SetMonitor. */
  i32 index = 0;
  std::string name;
  /** Left edge in desktop coordinates. */
  i32 x = 0;
  /** Top edge in desktop coordinates. */
  i32 y = 0;
  /** Current mode width in pixels. */
  u32 width = 0;
  /** Current mode height in pixels. */
  u32 height = 0;
  /** Current mode refresh rate in hertz. */
  f32 refresh_rate = 0;
  /** Ratio of window pixels to layout size. */
  f32 content_scale = 1;
};

struct HostLocale {
  /** Language code such as en or ja. */
  std::string language;
  /** Country code such as US, empty when the system gives none. */
  std::string country;
};

struct WindowConfig {
  std::string title;
  u32 width = 1280;
  u32 height = 720;
  u32 min_width = 0;
  u32 min_height = 0;
  ui::AspectRatio aspect_ratio = ui::AspectRatio::kStretch;
  bool fullscreen = true;
  bool fullscreen_exclusive = false; /**< Exclusive rather than borderless. */
  i32 monitor = 0;
  bool vulkan_surface = false; /**< The graphics driver presents through Vulkan. */
  u32 mode_width = 0;          /**< Exclusive fullscreen mode width, 0 for the desktop. */
  u32 mode_height = 0;         /**< Exclusive fullscreen mode height, 0 for the desktop. */
};

enum class CursorShape : u8 {
  kArrow,
  kTextInput,
  kResizeAll,
  kResizeNS,
  kResizeEW,
  kResizeNESW,
  kResizeNWSE,
  kHand,
  kNotAllowed,
};

}  // namespace rex::ui
