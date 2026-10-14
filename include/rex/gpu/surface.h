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

#include <cstdint>

#include <rex/assert.h>
#include <rex/types.h>

namespace rex {
namespace gpu {

/**
 * Presentation target. Methods are callable only from the UI thread.
 */
class Surface : public rex::NonCopyable {
 public:
  /**
   * Surface types. Within one platform the more preferable types come first so
   * rex::bit_scan_forward can try every type supported by both the graphics
   * provider and the window.
   */
  enum TypeIndex : u8 {
    kTypeIndexWaylandSurface, /**< Preferred over xcb, which needs XWayland there. */
    kTypeIndexXCBWindow,      /**< X11 window through the XCB library. */
    kTypeIndexWin32HWND,      /**< Windows window handle. */
    kTypeIndexCAMetalLayer,   /**< Presented via MoltenVK with VK_EXT_metal_surface. */
  };
  /** Bit mask of `TypeIndex` values, 1 bit per surface type. */
  using TypeFlags = u32;
  enum : u8 {
    /** Bit of kTypeIndexWaylandSurface. */
    kTypeFlagWaylandSurface = TypeFlags(1) << kTypeIndexWaylandSurface,
    kTypeFlagXCBWindow = TypeFlags(1) << kTypeIndexXCBWindow, /**< Bit of kTypeIndexXCBWindow. */
    kTypeFlagWin32HWND = TypeFlags(1) << kTypeIndexWin32HWND, /**< Bit of kTypeIndexWin32HWND. */
    /** Bit of kTypeIndexCAMetalLayer. */
    kTypeFlagCAMetalLayer = TypeFlags(1) << kTypeIndexCAMetalLayer,
  };

  virtual ~Surface() = default;

  /**
   * Returns the platform type of the surface.
   *
   * @return The `TypeIndex` of the concrete surface.
   */
  virtual TypeIndex GetType() const = 0;

  /**
   * Returns the up-to-date size in physical display pixels. The size should be
   * the surface dimensions without platform composition stretching, and any
   * stretching must keep surface pixels at a 1:1 aspect ratio to the display.
   *
   * @param width_out Receives the width, or 0 when not ready.
   * @param height_out Receives the height, or 0 when not ready.
   * @return True when ready to open a presentation connection. False when not
   *         ready or when either dimension is 0, since a zero area surface
   *         cannot be presented to.
   */
  bool GetSize(u32& width_out, u32& height_out) {
    u32 width, height;
    if (!GetSizeImpl(width, height) || !width || !height) {
      width_out = 0;
      height_out = 0;
      return false;
    }
    width_out = width;
    height_out = height;
    return true;
  }

 protected:
  Surface() = default;

  /**
   * Returns the up-to-date size in physical pixels.
   *
   * @param width_out Receives the width.
   * @param height_out Receives the height.
   * @return True when ready to open a presentation connection.
   */
  virtual bool GetSizeImpl(u32& width_out, u32& height_out) const = 0;
};

}  // namespace gpu
}  // namespace rex
