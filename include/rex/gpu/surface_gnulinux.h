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

#pragma once

#include <rex/gpu/surface.h>

#include <xcb/xcb.h>

struct SDL_Window;
struct wl_display;
struct wl_surface;

namespace rex {
namespace gpu {

/** Presentation target backed by an X11 window reached through XCB. */
class XCBWindowSurface final : public Surface {
 public:
  /**
   * Wraps an XCB window.
   *
   * @param connection The XCB connection that owns the window.
   * @param window The window to present to.
   */
  explicit XCBWindowSurface(xcb_connection_t* connection, xcb_window_t window)
      : connection_(connection), window_(window) {}
  TypeIndex GetType() const override { return kTypeIndexXCBWindow; }
  /** Returns the XCB connection. */
  xcb_connection_t* GetConnection() const { return connection_; }
  /** Returns the XCB window. */
  xcb_window_t GetWindow() const { return window_; }

 protected:
  /**
   * Reads the platform surface size.
   *
   * @param width_out Receives the width in physical display pixels.
   * @param height_out Receives the height in physical display pixels.
   * @return True when the size was read.
   */
  bool GetSizeImpl(u32& width_out, u32& height_out) const override;

 private:
  xcb_connection_t* connection_;
  xcb_window_t window_;
};

/** Presentation target backed by a Wayland surface. */
class WaylandSurface final : public Surface {
 public:
  /**
   * Wraps a Wayland surface.
   *
   * @param display The Wayland display connection.
   * @param surface The surface to present to.
   * @param sdl_window The SDL window that owns the surface.
   */
  explicit WaylandSurface(::wl_display* display, ::wl_surface* surface, SDL_Window* sdl_window)
      : display_(display), surface_(surface), sdl_window_(sdl_window) {}
  TypeIndex GetType() const override { return kTypeIndexWaylandSurface; }
  /** Returns the Wayland display connection. */
  ::wl_display* GetDisplay() const { return display_; }
  /** Returns the Wayland surface. */
  ::wl_surface* GetSurface() const { return surface_; }

 protected:
  /**
   * Reads the platform surface size.
   *
   * @param width_out Receives the width in physical display pixels.
   * @param height_out Receives the height in physical display pixels.
   * @return True when the size was read.
   */
  bool GetSizeImpl(u32& width_out, u32& height_out) const override;

 private:
  ::wl_display* display_;
  ::wl_surface* surface_;
  SDL_Window* sdl_window_;
};

}  // namespace gpu
}  // namespace rex
