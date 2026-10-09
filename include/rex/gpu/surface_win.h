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
#include <memory>

#include <rex/gpu/surface.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rex {
namespace gpu {

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_GAMES)
/** Presentation target backed by a Win32 window. */
class Win32HwndSurface final : public Surface {
 public:
  /**
   * Wraps a Win32 window.
   *
   * @param hinstance The module instance that owns the window.
   * @param hwnd The window to present to.
   */
  explicit Win32HwndSurface(HINSTANCE hinstance, HWND hwnd) : hinstance_(hinstance), hwnd_(hwnd) {}
  TypeIndex GetType() const override { return kTypeIndex_Win32Hwnd; }
  HINSTANCE hinstance() const { return hinstance_; }
  HWND hwnd() const { return hwnd_; }

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
  HINSTANCE hinstance_;
  HWND hwnd_;
};
#endif

}  // namespace gpu
}  // namespace rex
