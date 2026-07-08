/**
 * @file        ui/surface_mac.cpp
 * @brief       macOS CAMetalLayer surface shim for SDL3-backed windows
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/surface_mac.h>

#include <SDL3/SDL_video.h>

namespace rex::ui {

bool CAMetalLayerSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  if (!sdl_window_) {
    return false;
  }

  int width = 0;
  int height = 0;
  if (!SDL_GetWindowSizeInPixels(sdl_window_, &width, &height) || width <= 0 || height <= 0) {
    return false;
  }

  width_out = uint32_t(width);
  height_out = uint32_t(height);
  return true;
}

}  // namespace rex::ui
