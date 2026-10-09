/**
 * @file        ui/sdl3/sdl_virtual_key.h
 * @brief       SDL3 scancode to Win32-style hid::VirtualKey translation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <SDL3/SDL_scancode.h>

#include <rex/hid/virtual_key.h>
#include <rex/macros.h>

namespace rex::ui {

/**
 * Maps an SDL scancode to the Win32 style hid::VirtualKey used by the input listener API.
 *
 * @param scancode SDL scancode to map.
 * @return The virtual key, or hid::VirtualKey::kNone for unmapped keys.
 */
hid::VirtualKey TranslateSDLScancode(SDL_Scancode scancode);

}  // namespace rex::ui
