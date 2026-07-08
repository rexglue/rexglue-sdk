/**
 * @file        ui/window_sdl.h
 * @brief       SDL3 implementation of the Window abstraction
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

#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context_sdl.h>

namespace rex::ui {

class WindowSDL final : public Window {
 public:
  WindowSDL(WindowedAppContext& app_context, const std::string_view title,
            uint32_t desired_logical_width, uint32_t desired_logical_height);
  ~WindowSDL() override;

  void* GetNativeWindowHandle() const override;

  // Called by SDLWindowedAppContext on the UI thread.
  void HandleWindowEvent(SDL_Event& event);
  void HandleKeyEvent(SDL_Event& event);
  void HandleTextInputEvent(SDL_Event& event);
  void HandleMouseEvent(SDL_Event& event);
  void HandleDropEvent(SDL_Event& event);
  void HandlePaintEvent();

 protected:
  uint32_t GetLatestDpiImpl() const override;

  bool OpenImpl() override;
  void RequestCloseImpl() override;

  void ApplyNewFullscreen() override;
  void ApplyNewTitle() override;
  void ApplyNewMouseCapture() override;
  void ApplyNewMouseRelease() override;
  void ApplyNewCursorVisibility(CursorVisibility old_cursor_visibility) override;
  void FocusImpl() override;

  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

 private:
  SDLWindowedAppContext& sdl_app_context() const {
    return static_cast<SDLWindowedAppContext&>(app_context());
  }

  // Performs the common close choreography (OnBeforeClose, native destroy,
  // OnAfterClose). Used by both RequestCloseImpl and the close-requested
  // event handler.
  void PerformClose();
  void DestroySDLWindow();

#if REX_PLATFORM_MAC
  void DestroyMetalView();
  void* GetOrCreateMetalLayer();
#endif

  void ApplyCursorVisibilityNow();
  void RearmCursorAutoHideTimer();

  SDL_Window* sdl_window_ = nullptr;
  SDL_WindowID sdl_window_id_ = 0;
  std::atomic<bool> paint_pending_{false};
#if REX_PLATFORM_MAC
  void* sdl_metal_view_ = nullptr;
#endif
  // Auto-hide cursor bookkeeping (CursorVisibility::kAutoHidden).
  SDL_TimerID cursor_hide_timer_ = 0;
};

}  // namespace rex::ui
