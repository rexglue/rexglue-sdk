/**
 * @file        platform/native_window.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/types.h>

namespace rex {

struct NativeWindow {
  enum class Type : u8 {
    kNone,
    kWin32,   /**< window is an HWND. */
    kCocoa,   /**< window is an NSWindow pointer and layer a CAMetalLayer pointer. */
    kWayland, /**< window is a wl_surface pointer and display a wl_display pointer. */
    kX11,     /**< window is an X11 Window id stored as an integer and display a Display pointer. */
  };
  Type type = Type::kNone;
  void* window = nullptr;
  /** The display connection. Null on Win32 and Cocoa. */
  void* display = nullptr;
  /** The CAMetalLayer on Cocoa. Created on first request and owned by the window. */
  void* layer = nullptr;
  /** The SDL_Window of an SDL backed window on every platform, otherwise null. */
  void* sdl = nullptr;
};

}  // namespace rex
