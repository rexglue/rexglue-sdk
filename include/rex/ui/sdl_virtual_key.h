/**
 * @file        ui/sdl_virtual_key.h
 * @brief       SDL3 scancode to Win32-style VirtualKey translation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <SDL3/SDL_scancode.h>

#include <rex/ui/virtual_key.h>

namespace rex::ui {

/// Maps an SDL scancode to the Win32-style VirtualKey used by the input
/// listener API. Returns VirtualKey::kNone for unmapped keys.
VirtualKey TranslateSDLScancode(SDL_Scancode scancode);

/// Maps a Win32-style VirtualKey to an SDL scancode for polling
/// SDL_GetKeyboardState(). Returns SDL_SCANCODE_UNKNOWN for unmapped keys.
SDL_Scancode TranslateVirtualKeyToSDLScancode(VirtualKey vk);

}  // namespace rex::ui
