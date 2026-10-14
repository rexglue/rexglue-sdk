/**
 * @file        ui/sdl3/window_sdl.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's window_win.cc (Ben Vanik, 2020).
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>

#include <SDL3/SDL.h>

#include <rex/macros.h>
#include "ui/sdl3/window.h"
#include "ui/sdl3/app_context_sdl.h"

namespace rex::ui {

class WindowSDL final : public Window {
 public:
  WindowSDL(WindowedAppContext& app_context, const std::string_view title,
            u32 desired_logical_width, u32 desired_logical_height);
  ~WindowSDL() override;

  void Raise() override;
  NativeWindow GetNative() override;
  SDL_Window* GetSDL() const override;
  bool SetRelativeMouseMode(bool enable) override;
  bool WarpMouseToCenter(i32& x_out, i32& y_out) override;
  bool GetDisplayPixelSize(u32& width, u32& height) const override;

  /**
   * Handles a window event. SDLWindowedAppContext calls all Handle methods on the UI thread.
   *
   * @param event SDL window event.
   */
  void HandleWindowEvent(SDL_Event& event);
  void HandleKeyEvent(SDL_Event& event);
  void HandleTextInputEvent(SDL_Event& event);
  void HandleMouseEvent(SDL_Event& event);
  void HandleDropEvent(SDL_Event& event);
  void HandlePaintEvent();

 protected:
  u32 GetLatestDPIImpl() const override;

  bool OpenImpl() override;
  void RequestCloseImpl() override;

  void ApplyNewFullscreen() override;
  void ApplyNewMonitor() override;
  void ApplyNewDesiredLogicalSize() override;
  void ApplyNewTitle() override;
  void ApplyNewMouseCapture() override;
  void ApplyNewMouseRelease() override;
  void ApplyNewCursorVisibility(CursorVisibility old_cursor_visibility) override;
  void ApplyNewTextInputActive() override;
  void FocusImpl() override;
  void ApplySizeLimits() override;

  std::unique_ptr<gpu::Surface> CreateSurfaceImpl(gpu::Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

 private:
  SDLWindowedAppContext& GetSDLAppContext() const {
    return static_cast<SDLWindowedAppContext&>(GetAppContext());
  }

  /** Performs the common close sequence for RequestCloseImpl and the close request handler. */
  void PerformClose();
  void DestroySDLWindow();

  void ApplyFullscreenModeNow();
  void CenterOnConfiguredDisplay();
  void ApplyCursorVisibilityNow();
  void ApplyTextInputActiveNow();
  void RearmCursorAutoHideTimer();

  /**
   * Returns the ratio between SDL window coordinates and the physical pixels listeners expect.
   *
   * @return The ratio, never 0.
   */
  f32 GetPixelDensity() const;

#if REX_PLATFORM_MAC
  void* AcquireMetalLayer();
#endif

  SDL_Window* sdl_window_ = nullptr;
  SDL_WindowID sdl_window_id_ = 0;
  std::atomic<bool> paint_pending_{false};
  /** Timer that hides the cursor in the AutoHidden mode. */
  SDL_TimerID cursor_hide_timer_ = 0;
#if REX_PLATFORM_MAC
  void* metal_view_ = nullptr;
#endif
};

}  // namespace rex::ui
