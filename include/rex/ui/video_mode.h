/**
 * @file        ui/video_mode.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/types.h>

namespace rex::ui {

/** The display mode the title queries through VdQueryVideoMode and XGetVideoMode. */
struct VideoMode {
  /** Width in pixels. */
  u32 width = 1280;
  /** Height in pixels. */
  u32 height = 720;
  /** Refresh rate in hertz. */
  f32 refresh_rate = 60.0f;
};

}  // namespace rex::ui
