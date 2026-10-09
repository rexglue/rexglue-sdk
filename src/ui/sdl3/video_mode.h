/**
 * @file        ui/sdl3/video_mode.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/types.h>

namespace rex::ui::detail {

bool ResolveConfiguredSize(i32& width_out, i32& height_out);
void ConfiguredVideoMode(u32& width, u32& height);
f32 ConfiguredRefreshRate();

}  // namespace rex::ui::detail
