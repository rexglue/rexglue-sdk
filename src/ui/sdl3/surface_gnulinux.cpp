/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdlib>

#include <SDL3/SDL.h>

#include <rex/gpu/surface_gnulinux.h>

namespace rex {
namespace gpu {

bool XCBWindowSurface::GetSizeImpl(u32& width_out, u32& height_out) const {
  xcb_get_geometry_reply_t* reply =
      xcb_get_geometry_reply(connection_, xcb_get_geometry(connection_, window_), nullptr);
  if (!reply) {
    return false;
  }
  width_out = reply->width;
  height_out = reply->height;
  std::free(reply);
  return true;
}

bool WaylandSurface::GetSizeImpl(u32& width_out, u32& height_out) const {
  int w = 0, h = 0;
  if (!SDL_GetWindowSizeInPixels(sdl_window_, &w, &h) || w <= 0 || h <= 0) {
    return false;
  }
  width_out = static_cast<u32>(w);
  height_out = static_cast<u32>(h);
  return true;
}

}  // namespace gpu
}  // namespace rex
