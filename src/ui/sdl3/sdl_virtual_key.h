/**
 * @file        ui/sdl3/sdl_virtual_key.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <SDL3/SDL_scancode.h>

#include <rex/types.h>
#include <rex/platform.h>
#include <rex/input_keys.h>

namespace rex::ui::sdl3 {

/**
 * Maps an SDL scancode to a VirtualKey.
 *
 * @return The virtual key, or VirtualKey::kNone for unmapped keys.
 */
VirtualKey TranslateSDLScancode(SDL_Scancode scancode);

}  // namespace rex::ui::sdl3
