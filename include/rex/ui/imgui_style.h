/**
 * @file        ui/imgui_style.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <imgui.h>

#include <rex/types.h>
#include <rex/platform.h>
#include <rex/export.h>
#include <rex/ui/style.h>

namespace rex::ui {

/**
 * Sets every ImGui color slot, rounding, border and padding from the palette and metrics.
 *
 * @param imgui_style The style to write.
 * @param palette The color roles.
 * @param metrics The sizes.
 */
REX_IMGUI_API void ApplyDefaultStyle(ImGuiStyle& imgui_style, const Palette& palette,
                                     const Metrics& metrics);

}  // namespace rex::ui
