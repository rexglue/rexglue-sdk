/**
 * @file        ui/window_sdl.cpp
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

#include <rex/ui/window_sdl.h>

#include <cmath>
#include <cstring>
#include <filesystem>

#define UTF_CPP_CPLUSPLUS 201703L  // -fno-char8_t
#include <utf8.h>

#include <rex/cvar.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/flags.h>
#include <rex/ui/sdl_virtual_key.h>

#if REX_PLATFORM_WIN32
#include <rex/ui/surface_win.h>
#elif REX_PLATFORM_MAC
#include <CoreFoundation/CoreFoundation.h>
#include <SDL3/SDL_metal.h>

#include <rex/ui/surface_mac.h>
#else
#include <X11/Xlib-xcb.h>
#include <rex/ui/surface_gnulinux.h>
#endif

namespace rex::ui {

namespace {

// SDL timer callback (runs on SDL's timer thread): defer the actual hide to
// the UI thread. The deferred function only touches the global SDL cursor and
// the window's nonvirtual cursor-visibility getter; the window owns the timer
// and removes it before destroying the SDL window.
Uint32 CursorAutoHideTimerCallback(void* userdata, SDL_TimerID timer_id, Uint32 interval) {
  (void)timer_id;
  (void)interval;
  auto* window = static_cast<WindowSDL*>(userdata);
  window->app_context().CallInUIThreadDeferred([window] {
    if (window->GetCursorVisibility() == Window::CursorVisibility::kAutoHidden) {
      SDL_HideCursor();
    }
  });
  return 0;  // One-shot.
}

MouseEvent::Button TranslateSDLMouseButton(Uint8 button) {
  switch (button) {
    case SDL_BUTTON_LEFT:
      return MouseEvent::Button::kLeft;
    case SDL_BUTTON_RIGHT:
      return MouseEvent::Button::kRight;
    case SDL_BUTTON_MIDDLE:
      return MouseEvent::Button::kMiddle;
    case SDL_BUTTON_X1:
      return MouseEvent::Button::kX1;
    case SDL_BUTTON_X2:
      return MouseEvent::Button::kX2;
    default:
      return MouseEvent::Button::kNone;
  }
}

}  // namespace

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       const std::string_view title) {
  uint32_t width = 0;
  uint32_t height = 0;
  ResolveConfiguredLogicalSize(width, height);
  return std::make_unique<WindowSDL>(app_context, title, width, height);
}

WindowSDL::WindowSDL(WindowedAppContext& app_context, const std::string_view title,
                     uint32_t desired_logical_width, uint32_t desired_logical_height)
    : Window(app_context, title, desired_logical_width, desired_logical_height) {}

WindowSDL::~WindowSDL() {
  EnterDestructor();
  DestroySDLWindow();
}

bool WindowSDL::OpenImpl() {
  // SDL window coordinates are physical pixels on Windows and X11. Cocoa
  // uses logical points and applies the backing scale itself.
  SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
#if REX_PLATFORM_MAC
  int initial_width = int(GetDesiredLogicalWidth());
  int initial_height = int(GetDesiredLogicalHeight());
#else
  int initial_width = int(SizeToPhysical(GetDesiredLogicalWidth()));
  int initial_height = int(SizeToPhysical(GetDesiredLogicalHeight()));
#endif
  sdl_window_ = SDL_CreateWindow(GetTitle().c_str(), initial_width, initial_height, flags);
  if (!sdl_window_) {
    REXLOG_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
    return false;
  }
  sdl_window_id_ = SDL_GetWindowID(sdl_window_);
  sdl_app_context().RegisterWindow(sdl_window_id_, this);

  CenterOnConfiguredDisplay();

  if (IsFullscreen()) {
    ApplyFullscreenModeNow();
    SDL_SetWindowFullscreen(sdl_window_, true);
  }
#if REX_PLATFORM_MAC
  CFPreferencesSetAppValue(CFSTR("ApplePressAndHoldEnabled"), kCFBooleanFalse,
                           kCFPreferencesCurrentApplication);
  CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication);
#endif
  // SDL3 requires explicit opt in for text input events. Reapplied from the
  // desired state so a reopened window comes back with the state it had.
  ApplyTextInputActiveNow();
  ApplyCursorVisibilityNow();
  SDL_ShowWindow(sdl_window_);

  // Actualize state for the common Window code. Listener dispatch is handled
  // by Window::Open after OpenImpl returns; these only record initial state.
  int pixel_width = 0;
  int pixel_height = 0;
  SDL_GetWindowSizeInPixels(sdl_window_, &pixel_width, &pixel_height);
  WindowDestructionReceiver destruction_receiver(this);
  OnActualSizeUpdate(uint32_t(pixel_width), uint32_t(pixel_height), destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return true;
  }
  if (SDL_GetWindowFlags(sdl_window_) & SDL_WINDOW_INPUT_FOCUS) {
    OnFocusUpdate(true, destruction_receiver);
  }
  return true;
}

void WindowSDL::RequestCloseImpl() {
  PerformClose();
}

void WindowSDL::PerformClose() {
  WindowDestructionReceiver destruction_receiver(this);
  OnBeforeClose(destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
  DestroySDLWindow();
  OnAfterClose();
}

void WindowSDL::DestroySDLWindow() {
  if (cursor_hide_timer_) {
    SDL_RemoveTimer(cursor_hide_timer_);
    cursor_hide_timer_ = 0;
  }
  if (sdl_window_) {
    sdl_app_context().UnregisterWindow(sdl_window_id_);
    SDL_DestroyWindow(sdl_window_);
    sdl_window_ = nullptr;
    sdl_window_id_ = 0;
  }
}

void* WindowSDL::GetNativeWindowHandle() const {
#if REX_PLATFORM_WIN32
  if (!sdl_window_) {
    return nullptr;
  }
  return SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_window_),
                                SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
  return nullptr;
#endif
}

bool WindowSDL::SetRelativeMouseMode(bool enable) {
  if (!sdl_window_) {
    return false;
  }
  if (!SDL_SetWindowRelativeMouseMode(sdl_window_, enable)) {
    REXLOG_WARN("SDL_SetWindowRelativeMouseMode({}) failed: {}", enable, SDL_GetError());
    return false;
  }
  return enable;
}

bool WindowSDL::WarpMouseToCenter(int32_t& x_out, int32_t& y_out) {
  if (!sdl_window_) {
    return false;
  }
  int width = 0;
  int height = 0;
  SDL_GetWindowSize(sdl_window_, &width, &height);
  if (width <= 0 || height <= 0) {
    return false;
  }
  float center_x = float(width) * 0.5f;
  float center_y = float(height) * 0.5f;
  SDL_WarpMouseInWindow(sdl_window_, center_x, center_y);
  // The warp reports nothing back and compositors may drop it, so confirm.
  float actual_x = 0.0f;
  float actual_y = 0.0f;
  SDL_GetMouseState(&actual_x, &actual_y);
  if (std::fabs(actual_x - center_x) > 1.0f || std::fabs(actual_y - center_y) > 1.0f) {
    return false;
  }
  float density = GetPixelDensity();
  x_out = int32_t(center_x * density);
  y_out = int32_t(center_y * density);
  return true;
}

bool WindowSDL::GetDisplayPixelSize(uint32_t& width, uint32_t& height) const {
  if (!sdl_window_) {
    return false;
  }
  const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(sdl_window_));
  if (!mode) {
    return false;
  }
  width = uint32_t(float(mode->w) * mode->pixel_density);
  height = uint32_t(float(mode->h) * mode->pixel_density);
  return width > 0 && height > 0;
}

float WindowSDL::GetPixelDensity() const {
  float density = sdl_window_ ? SDL_GetWindowPixelDensity(sdl_window_) : 1.0f;
  return density > 0.0f ? density : 1.0f;
}

uint32_t WindowSDL::GetLatestDpiImpl() const {
  float scale = sdl_window_ ? SDL_GetWindowDisplayScale(sdl_window_)
                            : SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  if (scale <= 0.0f) {
    return GetMediumDpi();
  }
  return uint32_t(scale * float(GetMediumDpi()) + 0.5f);
}

void WindowSDL::ApplyNewFullscreen() {
  if (!sdl_window_) {
    return;
  }
  ApplyFullscreenModeNow();
  SDL_SetWindowFullscreen(sdl_window_, IsFullscreen());
}

void WindowSDL::ApplyNewMonitor() {
  if (!sdl_window_) {
    return;
  }
  const bool was_fullscreen = IsFullscreen();
  if (was_fullscreen) {
    SDL_SetWindowFullscreen(sdl_window_, false);
    SDL_SyncWindow(sdl_window_);
  }
  CenterOnConfiguredDisplay();
  if (was_fullscreen) {
    ApplyFullscreenModeNow();
    SDL_SetWindowFullscreen(sdl_window_, true);
  }
}

void WindowSDL::ApplyNewDesiredLogicalSize() {
  if (!sdl_window_ || (SDL_GetWindowFlags(sdl_window_) &
                       (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED))) {
    return;
  }
#if REX_PLATFORM_MAC
  SDL_SetWindowSize(sdl_window_, int(GetDesiredLogicalWidth()), int(GetDesiredLogicalHeight()));
#else
  SDL_SetWindowSize(sdl_window_, int(SizeToPhysical(GetDesiredLogicalWidth())),
                    int(SizeToPhysical(GetDesiredLogicalHeight())));
#endif
}

void WindowSDL::CenterOnConfiguredDisplay() {
  const int32_t monitor_index = GetMonitor();
  if (!sdl_window_ || monitor_index <= 0) {
    return;
  }
  int display_count = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&display_count);
  if (!displays) {
    return;
  }
  if (monitor_index <= display_count) {
    SDL_DisplayID display = displays[monitor_index - 1];
    SDL_SetWindowPosition(sdl_window_, SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                          SDL_WINDOWPOS_CENTERED_DISPLAY(display));
  } else {
    REXLOG_WARN("monitor cvar is {} but only {} display(s) present; using default", monitor_index,
                display_count);
  }
  SDL_free(displays);
}

void WindowSDL::ApplyFullscreenModeNow() {
  if (!sdl_window_) {
    return;
  }
  if (!REXCVAR_GET(fullscreen_exclusive)) {
    SDL_SetWindowFullscreenMode(sdl_window_, nullptr);
    return;
  }

  const SDL_DisplayID display = SDL_GetDisplayForWindow(sdl_window_);
  int32_t width = 0;
  int32_t height = 0;
  if (!rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(width, height) ||
      width <= 0 || height <= 0) {
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    if (!desktop) {
      REXLOG_WARN("No desktop mode for display {}: staying borderless", uint32_t(display));
      SDL_SetWindowFullscreenMode(sdl_window_, nullptr);
      return;
    }
    width = desktop->w;
    height = desktop->h;
  }

  SDL_DisplayMode mode = {};
  if (!SDL_GetClosestFullscreenDisplayMode(display, width, height, 0.0f, true, &mode)) {
    REXLOG_WARN("Display {} has no mode near {}x{}: staying borderless", uint32_t(display), width,
                height);
    SDL_SetWindowFullscreenMode(sdl_window_, nullptr);
    return;
  }
  if (!SDL_SetWindowFullscreenMode(sdl_window_, &mode)) {
    REXLOG_WARN("SDL_SetWindowFullscreenMode({}x{}) failed: {}", mode.w, mode.h, SDL_GetError());
    return;
  }
  REXLOG_INFO("Exclusive fullscreen mode {}x{} @ {:.3g}Hz on display {}", mode.w, mode.h,
              mode.refresh_rate, uint32_t(display));
}

void WindowSDL::ApplyNewTitle() {
  if (!sdl_window_) {
    return;
  }
  SDL_SetWindowTitle(sdl_window_, GetTitle().c_str());
}

void WindowSDL::ApplyNewMouseCapture() {
  SDL_CaptureMouse(true);
}

void WindowSDL::ApplyNewMouseRelease() {
  SDL_CaptureMouse(false);
}

void WindowSDL::ApplyNewTextInputActive() {
  ApplyTextInputActiveNow();
}

void WindowSDL::ApplyTextInputActiveNow() {
  if (!sdl_window_) {
    return;
  }
  if (IsTextInputActive()) {
    SDL_StartTextInput(sdl_window_);
  } else {
    SDL_StopTextInput(sdl_window_);
  }
}

void WindowSDL::ApplyNewCursorVisibility(CursorVisibility old_cursor_visibility) {
  (void)old_cursor_visibility;
  ApplyCursorVisibilityNow();
}

void WindowSDL::ApplyCursorVisibilityNow() {
  switch (GetCursorVisibility()) {
    case CursorVisibility::kVisible:
      if (cursor_hide_timer_) {
        SDL_RemoveTimer(cursor_hide_timer_);
        cursor_hide_timer_ = 0;
      }
      SDL_ShowCursor();
      break;
    case CursorVisibility::kHidden:
      if (cursor_hide_timer_) {
        SDL_RemoveTimer(cursor_hide_timer_);
        cursor_hide_timer_ = 0;
      }
      SDL_HideCursor();
      break;
    case CursorVisibility::kAutoHidden:
      // Hide immediately (see the contract in window.h: switching to
      // kAutoHidden hides instantly, e.g. when entering fullscreen); the
      // mouse-motion handler reveals the cursor and re-arms the timer.
      SDL_HideCursor();
      break;
  }
}

void WindowSDL::RearmCursorAutoHideTimer() {
  if (cursor_hide_timer_) {
    SDL_RemoveTimer(cursor_hide_timer_);
  }
  cursor_hide_timer_ = SDL_AddTimer(GetCursorAutoHideDelayMs(), CursorAutoHideTimerCallback, this);
}

void WindowSDL::FocusImpl() {
  if (!sdl_window_) {
    return;
  }
  SDL_RaiseWindow(sdl_window_);
}

std::unique_ptr<Surface> WindowSDL::CreateSurfaceImpl(Surface::TypeFlags allowed_types) {
  if (!sdl_window_) {
    return nullptr;
  }
#if REX_PLATFORM_WIN32
  SDL_PropertiesID props = SDL_GetWindowProperties(sdl_window_);
  if (allowed_types & Surface::kTypeFlag_Win32Hwnd) {
    HWND hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    HINSTANCE hinstance = static_cast<HINSTANCE>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr));
    if (hwnd) {
      return std::make_unique<Win32HwndSurface>(hinstance, hwnd);
    }
  }
#elif REX_PLATFORM_MAC
  if (allowed_types & Surface::kTypeFlag_CAMetalLayer) {
    SDL_MetalView metal_view = SDL_Metal_CreateView(sdl_window_);
    if (metal_view) {
      void* layer = SDL_Metal_GetLayer(metal_view);
      if (layer) {
        return std::make_unique<CAMetalLayerSurface>(sdl_window_, metal_view, layer);
      }
      SDL_Metal_DestroyView(metal_view);
    }
  }
#else
  SDL_PropertiesID props = SDL_GetWindowProperties(sdl_window_);
  if (allowed_types & Surface::kTypeFlag_WaylandSurface) {
    auto* wl_display_ptr = static_cast<struct wl_display*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
    auto* wl_surface_ptr = static_cast<struct wl_surface*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
    if (wl_display_ptr && wl_surface_ptr) {
      return std::make_unique<WaylandSurface>(wl_display_ptr, wl_surface_ptr, sdl_window_);
    }
  }
  if (allowed_types & Surface::kTypeFlag_XcbWindow) {
    auto* display = static_cast<Display*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
    auto x11_window = static_cast<xcb_window_t>(
        SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    if (display && x11_window) {
      return std::make_unique<XcbWindowSurface>(XGetXCBConnection(display), x11_window);
    }
  }
#endif
  return nullptr;
}

void WindowSDL::RequestPaintImpl() {
  // Coalesce: at most one queued paint event at a time. Callable from non-UI
  // threads; SDL_PushEvent is thread-safe.
  if (paint_pending_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  SDL_Event event{};
  event.type = sdl_app_context().paint_event_type();
  event.user.windowID = sdl_window_id_;
  SDL_PushEvent(&event);
}

void WindowSDL::HandlePaintEvent() {
  paint_pending_.store(false, std::memory_order_release);
  OnPaint();
}

void WindowSDL::HandleWindowEvent(SDL_Event& event) {
  WindowDestructionReceiver destruction_receiver(this);
  switch (event.type) {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      OnActualSizeUpdate(uint32_t(event.window.data1), uint32_t(event.window.data2),
                         destruction_receiver);
      break;
    case SDL_EVENT_WINDOW_RESIZED: {
      // Track the user-driven size as the desired size for the normal state
      // only (mirrors the Win32 WM_SIZE handling).
      SDL_WindowFlags flags = SDL_GetWindowFlags(sdl_window_);
      if (!(flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED))) {
#if REX_PLATFORM_MAC
        // Cocoa reports the client size in logical points. Converting it from
        // the backing DPI a second time would halve the desired size on Retina
        // displays.
        OnDesiredLogicalSizeUpdate(uint32_t(event.window.data1), uint32_t(event.window.data2));
#else
        OnDesiredLogicalSizeUpdate(SizeToLogical(uint32_t(event.window.data1)),
                                   SizeToLogical(uint32_t(event.window.data2)));
#endif
      }
      break;
    }
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
      UISetupEvent e(this);
      OnDpiChanged(e, destruction_receiver);
      break;
    }
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED: {
      MonitorUpdateEvent e(this, true);
      OnMonitorUpdate(e);
      break;
    }
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      OnFocusUpdate(true, destruction_receiver);
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      OnFocusUpdate(false, destruction_receiver);
      break;
    case SDL_EVENT_WINDOW_EXPOSED:
      // The platform cannot retain the previous image; force the paint.
      OnPaint(true);
      break;
    case SDL_EVENT_WINDOW_MINIMIZED:
      OnMinimized(destruction_receiver);
      break;
    case SDL_EVENT_WINDOW_RESTORED:
      OnRestored(destruction_receiver);
      break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      // SDL destroys nothing on its own; this is the veto point.
      if (SendCloseRequestToListeners(destruction_receiver)) {
        if (!destruction_receiver.IsWindowDestroyed()) {
          PerformClose();
        }
      }
      break;
    default:
      break;
  }
}

void WindowSDL::HandleDropEvent(SDL_Event& event) {
  WindowDestructionReceiver destruction_receiver(this);
  FileDropEvent e(this, std::filesystem::path(event.drop.data));
  OnFileDrop(e, destruction_receiver);
}

void WindowSDL::HandleKeyEvent(SDL_Event& event) {
  VirtualKey virtual_key = TranslateSDLScancode(event.key.scancode);
  if (virtual_key == VirtualKey::kNone) {
    return;
  }
  SDL_Keymod mod = event.key.mod;
  KeyEvent e(this, virtual_key, /*repeat_count=*/1,
             /*prev_state=*/event.key.repeat,
             /*modifier_shift_pressed=*/(mod & SDL_KMOD_SHIFT) != 0,
             /*modifier_ctrl_pressed=*/(mod & SDL_KMOD_CTRL) != 0,
             /*modifier_alt_pressed=*/(mod & SDL_KMOD_ALT) != 0,
             /*modifier_super_pressed=*/(mod & SDL_KMOD_GUI) != 0);
  WindowDestructionReceiver destruction_receiver(this);
  if (event.type == SDL_EVENT_KEY_DOWN) {
    OnKeyDown(e, destruction_receiver);
  } else {
    OnKeyUp(e, destruction_receiver);
  }
}

void WindowSDL::HandleTextInputEvent(SDL_Event& event) {
  // Replicate the Win32 WM_CHAR behavior: one OnKeyChar per codepoint with
  // the character code in the virtual key slot.
  const char* text = event.text.text;
  if (!text || !*text) {
    return;
  }
  WindowDestructionReceiver destruction_receiver(this);
  const char* it = text;
  const char* end = text + std::strlen(text);
  while (it < end) {
    uint32_t codepoint = utf8::unchecked::next(it);
    KeyEvent e(this, VirtualKey(codepoint), 1, false, false, false, false, false);
    OnKeyChar(e, destruction_receiver);
    if (destruction_receiver.IsWindowDestroyed()) {
      return;
    }
  }
}

void WindowSDL::HandleMouseEvent(SDL_Event& event) {
  // SDL3 reports float window coordinates; listeners expect physical pixels.
  float density = GetPixelDensity();
  WindowDestructionReceiver destruction_receiver(this);
  switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION: {
      if (GetCursorVisibility() == CursorVisibility::kAutoHidden) {
        SDL_ShowCursor();
        RearmCursorAutoHideTimer();
      }
      MouseEvent e(this, MouseEvent::Button::kNone, int32_t(event.motion.x * density),
                   int32_t(event.motion.y * density), 0, 0, event.motion.xrel * density,
                   event.motion.yrel * density);
      OnMouseMove(e, destruction_receiver);
      break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      MouseEvent e(this, TranslateSDLMouseButton(event.button.button),
                   int32_t(event.button.x * density), int32_t(event.button.y * density));
      if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        OnMouseDown(e, destruction_receiver);
      } else {
        OnMouseUp(e, destruction_receiver);
      }
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      MouseEvent e(this, MouseEvent::Button::kNone, int32_t(event.wheel.mouse_x * density),
                   int32_t(event.wheel.mouse_y * density),
                   int32_t(event.wheel.x * float(MouseEvent::kScrollPerDetent)),
                   int32_t(event.wheel.y * float(MouseEvent::kScrollPerDetent)));
      OnMouseWheel(e, destruction_receiver);
      break;
    }
    default:
      break;
  }
}

}  // namespace rex::ui
