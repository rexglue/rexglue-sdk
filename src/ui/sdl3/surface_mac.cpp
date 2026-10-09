/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Rien Gupta, 2026 - Adapted for ReXGlue runtime
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/gpu/surface_mac.h>

#include <SDL3/SDL_video.h>

namespace rex {
namespace gpu {

bool CAMetalLayerSurface::GetSizeImpl(u32& width_out, u32& height_out) const {
  int pixel_width = 0;
  int pixel_height = 0;
  if (!SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height)) {
    return false;
  }
  width_out = static_cast<u32>(pixel_width);
  height_out = static_cast<u32>(pixel_height);
  return true;
}

}  // namespace gpu
}  // namespace rex
