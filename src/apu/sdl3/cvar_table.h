/**
 * @file        apu/sdl3/cvar_table.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/cvar.h>

namespace rex::apu::sdl3::cvars {

REXCVAR_BOOL(apu, mute, false, "Mute audio output");
REXCVAR_TABLE(kSDL3Cvars, mute);

}  // namespace rex::apu::sdl3::cvars
