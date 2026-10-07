/**
 * @file        ui/color.h
 * @brief       Conversions between rex::Color and the ImGui color types
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <imgui.h>

#include <rex/color.h>

namespace rex::ui {

/**
 * Converts a color to the ImGui floating point form.
 *
 * @param color The color to convert.
 * @return The color with the same 4 channels.
 */
inline ImVec4 ToImVec4(Color color) {
  return ImVec4(color.r, color.g, color.b, color.a);
}

/**
 * Converts a color to the packed ImGui form, red in the low byte.
 *
 * @param color The color to convert.
 * @return The color packed by Color::ToRGBA8.
 */
inline ImU32 ToImU32(Color color) {
  return color.ToRGBA8();
}

/**
 * Converts an ImGui floating point color.
 *
 * @param value The ImGui color.
 * @return The color with the same 4 channels.
 */
inline Color FromImVec4(const ImVec4& value) {
  return Color::RGBA(value.x, value.y, value.z, value.w);
}

}  // namespace rex::ui
