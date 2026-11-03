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

#include <string_view>

#include <rex/types.h>
#include <rex/ui/window.h>
#include <rex/ui/ui_system.h>

namespace rex::ui::detail {

bool ResolveConfiguredSize(const UISettings& ui, i32& width_out, i32& height_out);
void ResolveModeSize(const UISettings& ui, u32& width_out, u32& height_out);
AspectRatio ParseAspectRatio(std::string_view text);

}  // namespace rex::ui::detail
