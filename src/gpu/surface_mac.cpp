/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Rien Gupta, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/gpu/surface_mac.h>

#include <SDL3/SDL_metal.h>
#include <SDL3/SDL_video.h>

namespace rex {
namespace gpu {

CAMetalLayerSurface::~CAMetalLayerSurface() {
  if (metal_view_) {
    SDL_Metal_DestroyView(static_cast<SDL_MetalView>(metal_view_));
    metal_view_ = nullptr;
  }
}

bool CAMetalLayerSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  int pixel_width = 0;
  int pixel_height = 0;
  if (!SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height)) {
    return false;
  }
  width_out = uint32_t(pixel_width);
  height_out = uint32_t(pixel_height);
  return true;
}

}  // namespace gpu
}  // namespace rex
