/**
 * @file        ui/sdl3/window_flags.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <SDL3/SDL.h>

#include <rex/gpu/backend_select.h>

namespace rex::ui {

/**
 * Returns the flags a window is created with. A Vulkan window loads the Vulkan library when it is
 * created, so the instance extension query that precedes the first surface sees the extensions of
 * the display server SDL picked.
 *
 * @param api The host API the window presents with.
 * @return The SDL window flags.
 */
inline SDL_WindowFlags GetWindowFlags(gpu::HostAPI api) {
  SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
  if (api == gpu::HostAPI::kVulkan) {
    flags |= SDL_WINDOW_VULKAN;
  }
  return flags;
}

}  // namespace rex::ui
