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

#include <atomic>
#include <cstdint>

#include <rex/assert.h>
#include <rex/types.h>
#include <rex/bit.h>
#include <rex/ui/native_window.h>
#include <rex/utility.h>

namespace rex {
namespace gpu {

/** Presentation target. Methods are callable only from the UI thread. */
class Surface : public rex::NonCopyable {
 public:
  /**
   * Surface types. Within one platform the more preferable types come first so
   * rex::bit_scan_forward can try every type supported by both the graphics
   * provider and the window.
   */
  enum TypeIndex : u8 {
    kTypeIndexWaylandSurface, /**< Preferred over xcb, which needs XWayland there. */
    kTypeIndexXCBWindow,
    kTypeIndexWin32HWND,
    kTypeIndexCAMetalLayer, /**< Presented via MoltenVK with VK_EXT_metal_surface. */
  };
  /** Bit mask of `TypeIndex` values, 1 bit per surface type. */
  using TypeFlags = u32;
  enum : u8 {
    kTypeFlagWaylandSurface = static_cast<TypeFlags>(1) << kTypeIndexWaylandSurface,
    kTypeFlagXCBWindow = static_cast<TypeFlags>(1) << kTypeIndexXCBWindow,
    kTypeFlagWin32HWND = static_cast<TypeFlags>(1) << kTypeIndexWin32HWND,
    kTypeFlagCAMetalLayer = static_cast<TypeFlags>(1) << kTypeIndexCAMetalLayer,
  };

  virtual ~Surface() = default;

  virtual TypeIndex GetType() const = 0;

  /**
   * Returns the current size in physical display pixels. The size should be the surface dimensions
   * without platform composition stretching, and any stretching must keep surface pixels at a 1:1
   * aspect ratio to the display. It returns true when ready to open a presentation connection,
   * false when not ready or when either dimension is 0.
   *
   * @param width_out Receives the width, or 0 when not ready.
   * @param height_out Receives the height, or 0 when not ready.
   */
  bool GetSize(u32& width_out, u32& height_out) {
    u32 width, height;
    if (!ReadSize(width, height) || !width || !height) {
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
   * @param width_out Receives the width.
   * @param height_out Receives the height.
   * @return True when ready to open a presentation connection.
   */
  virtual bool ReadSize(u32& width_out, u32& height_out) const = 0;
};

/**
 * The surface of a UISystem window. Reports the size it was last given, so
 * no platform query runs on the presenting thread.
 */
class NativeSurface final : public Surface {
 public:
  explicit NativeSurface(const NativeWindow& window);

  TypeIndex GetType() const override;

  /**
   * Sets the size reported to the presenter.
   *
   * @param width The width in pixels.
   * @param height The height in pixels.
   */
  void SetSize(u32 width, u32 height) {
    size_.store((static_cast<u64>(width) << 32) | height, std::memory_order_release);
  }

  void* GetHWND() const { return window_.window; }

  /** Returns the module instance of the process, null elsewhere. */
  void* GetHINSTANCE() const;

  /** The CAMetalLayer pointer on macOS. */
  void* GetLayer() const { return window_.layer; }

  /**
   * @return The wl_display pointer on Wayland, the Display pointer on X11.
   */
  void* GetDisplay() const { return window_.display; }

  /**
   * @return The X11 window id.
   */
  u32 GetWindowID() const {
    return static_cast<u32>(reinterpret_cast<std::uintptr_t>(window_.window));
  }

  const NativeWindow& GetWindow() const { return window_; }

 protected:
  bool ReadSize(u32& width_out, u32& height_out) const override {
    const u64 size = size_.load(std::memory_order_acquire);
    width_out = static_cast<u32>(size >> 32);
    height_out = static_cast<u32>(size);
    return true;
  }

 private:
  NativeWindow window_;
  std::atomic<u64> size_{0};
};

}  // namespace gpu
}  // namespace rex
