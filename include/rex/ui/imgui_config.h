/**
 * @file        ui/imgui_config.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/types.h>
#include <rex/platform.h>

/**
 * Marks the ImGui API, which rexbase exports and every other binary imports.
 *
 * @code
 * IMGUI_API bool Begin(const char* name, bool* p_open, ImGuiWindowFlags flags);
 * @endcode
 */
#define IMGUI_API REX_API
