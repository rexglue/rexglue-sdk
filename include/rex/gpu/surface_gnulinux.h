#pragma once
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

#include <rex/gpu/surface.h>

#include <xcb/xcb.h>

struct SDL_Window;
struct wl_display;
struct wl_surface;

namespace rex {
namespace gpu {

class XcbWindowSurface final : public Surface {
 public:
  explicit XcbWindowSurface(xcb_connection_t* connection, xcb_window_t window)
      : connection_(connection), window_(window) {}
  TypeIndex GetType() const override { return kTypeIndex_XcbWindow; }
  xcb_connection_t* connection() const { return connection_; }
  xcb_window_t window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  xcb_connection_t* connection_;
  xcb_window_t window_;
};

class WaylandSurface final : public Surface {
 public:
  explicit WaylandSurface(::wl_display* display, ::wl_surface* surface, SDL_Window* sdl_window)
      : display_(display), surface_(surface), sdl_window_(sdl_window) {}
  TypeIndex GetType() const override { return kTypeIndex_WaylandSurface; }
  ::wl_display* display() const { return display_; }
  ::wl_surface* surface() const { return surface_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  ::wl_display* display_;
  ::wl_surface* surface_;
  SDL_Window* sdl_window_;
};

}  // namespace gpu
}  // namespace rex
