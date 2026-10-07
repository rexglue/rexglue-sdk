/**
 * @file        ui/imgui_config.h
 * @brief       ImGui build configuration of the SDK
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/macros.h>

/**
 * Marks the ImGui API, which rex-imgui exports and every other binary imports.
 *
 * @code
 * IMGUI_API bool Begin(const char* name, bool* p_open, ImGuiWindowFlags flags);
 * @endcode
 */
#define IMGUI_API REX_IMGUI_API
