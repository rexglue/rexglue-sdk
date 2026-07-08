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

#include <algorithm>
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
#include <SDL3/SDL_metal.h>
#include <rex/ui/surface_mac.h>
#else
#include <X11/Xlib-xcb.h>
#include <rex/ui/surface_gnulinux.h>
#endif

namespace rex::ui {

namespace {

uint32_t ResolveWindowWidth(uint32_t requested_width) {
  if (REXCVAR_GET(window_width) > 0) {
    return uint32_t(REXCVAR_GET(window_width));
  }
  if (!rex::cvar::HasNonDefaultValue("window_width")) {
    if (rex::cvar::HasNonDefaultValue("video_mode_width") && REXCVAR_GET(video_mode_width) > 0) {
      return uint32_t(std::clamp(REXCVAR_GET(video_mode_width), 1, 8192));
    }
    int32_t preset_width = 0;
    int32_t preset_height = 0;
    if (rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(preset_width,
                                                                       preset_height)) {
      return uint32_t(std::clamp(preset_width, 1, 8192));
    }
  }
  return requested_width;
}

uint32_t ResolveWindowHeight(uint32_t requested_height) {
  if (REXCVAR_GET(window_height) > 0) {
    return uint32_t(REXCVAR_GET(window_height));
  }
  if (!rex::cvar::HasNonDefaultValue("window_height")) {
    if (rex::cvar::HasNonDefaultValue("video_mode_height") && REXCVAR_GET(video_mode_height) > 0) {
      return uint32_t(std::clamp(REXCVAR_GET(video_mode_height), 1, 8192));
    }
    int32_t preset_width = 0;
    int32_t preset_height = 0;
    if (rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(preset_width,
                                                                       preset_height)) {
      return uint32_t(std::clamp(preset_height, 1, 8192));
    }
  }
  return requested_height;
}

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
                                       const std::string_view title, uint32_t desired_logical_width,
                                       uint32_t desired_logical_height) {
  desired_logical_width = ResolveWindowWidth(desired_logical_width);
  desired_logical_height = ResolveWindowHeight(desired_logical_height);
  return std::make_unique<WindowSDL>(app_context, title, desired_logical_width,
                                     desired_logical_height);
}

WindowSDL::WindowSDL(WindowedAppContext& app_context, const std::string_view title,
                     uint32_t desired_logical_width, uint32_t desired_logical_height)
    : Window(app_context, title, desired_logical_width, desired_logical_height) {}

WindowSDL::~WindowSDL() {
  EnterDestructor();
  DestroySDLWindow();
}

bool WindowSDL::OpenImpl() {
  // SDL window coordinates are physical pixels on Windows and X11.
  SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
#if REX_PLATFORM_MAC
  flags |= SDL_WINDOW_METAL;
#endif
  sdl_window_ = SDL_CreateWindow(GetTitle().c_str(), int(SizeToPhysical(GetDesiredLogicalWidth())),
                                 int(SizeToPhysical(GetDesiredLogicalHeight())), flags);
  if (!sdl_window_) {
    REXLOG_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
    return false;
  }
  sdl_window_id_ = SDL_GetWindowID(sdl_window_);
  sdl_app_context().RegisterWindow(sdl_window_id_, this);

#if REX_PLATFORM_MAC
  // Create the Metal view before any fullscreen/display sizing changes so the
  // drawable tracks the initial transition as part of SDL's native lifecycle.
  if (!GetOrCreateMetalLayer()) {
    DestroySDLWindow();
    return false;
  }
#endif

  // Center on the requested display before fullscreen so SDL resolves
  // fullscreen against it. 1-based enumeration order; 0 = system default.
  if (int32_t monitor_index = REXCVAR_GET(monitor); monitor_index > 0) {
    int display_count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&display_count);
    if (displays) {
      if (monitor_index <= display_count) {
        SDL_DisplayID display = displays[monitor_index - 1];
        SDL_SetWindowPosition(sdl_window_, SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                              SDL_WINDOWPOS_CENTERED_DISPLAY(display));
      } else {
        REXLOG_WARN("monitor cvar is {} but only {} display(s) present; using default",
                    monitor_index, display_count);
      }
      SDL_free(displays);
    }
  }

  if (IsFullscreen()) {
    // Borderless desktop fullscreen (a NULL display mode is SDL3's default).
    SDL_SetWindowFullscreen(sdl_window_, true);
  }
  // SDL3 requires explicit opt-in for text input events.
  SDL_StartTextInput(sdl_window_);
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
#if REX_PLATFORM_MAC
  DestroyMetalView();
#endif
  if (sdl_window_) {
    sdl_app_context().UnregisterWindow(sdl_window_id_);
    SDL_DestroyWindow(sdl_window_);
    sdl_window_ = nullptr;
    sdl_window_id_ = 0;
  }
}

#if REX_PLATFORM_MAC
void WindowSDL::DestroyMetalView() {
  if (!sdl_metal_view_) {
    return;
  }
  SDL_Metal_DestroyView(static_cast<SDL_MetalView>(sdl_metal_view_));
  sdl_metal_view_ = nullptr;
}

void* WindowSDL::GetOrCreateMetalLayer() {
  if (!sdl_window_) {
    return nullptr;
  }
  if (!sdl_metal_view_) {
    sdl_metal_view_ = SDL_Metal_CreateView(sdl_window_);
    if (!sdl_metal_view_) {
      REXLOG_ERROR("SDL_Metal_CreateView failed: {}", SDL_GetError());
      return nullptr;
    }
  }
  void* layer = SDL_Metal_GetLayer(static_cast<SDL_MetalView>(sdl_metal_view_));
  if (!layer) {
    REXLOG_ERROR("SDL_Metal_GetLayer failed: {}", SDL_GetError());
  }
  return layer;
}
#endif

void* WindowSDL::GetNativeWindowHandle() const {
  if (!sdl_window_) {
    return nullptr;
  }
#if REX_PLATFORM_WIN32
  return SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_window_),
                                SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif REX_PLATFORM_MAC
  return SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_window_),
                                SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#else
  return nullptr;
#endif
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
  SDL_SetWindowFullscreen(sdl_window_, IsFullscreen());
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
  if (allowed_types & Surface::kTypeFlag_Win32Hwnd) {
    SDL_PropertiesID props = SDL_GetWindowProperties(sdl_window_);
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
    if (void* layer = GetOrCreateMetalLayer()) {
      return std::make_unique<CAMetalLayerSurface>(sdl_window_, layer);
    }
  }
#else
  if (allowed_types & Surface::kTypeFlag_XcbWindow) {
    SDL_PropertiesID props = SDL_GetWindowProperties(sdl_window_);
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
        OnDesiredLogicalSizeUpdate(SizeToLogical(uint32_t(event.window.data1)),
                                   SizeToLogical(uint32_t(event.window.data2)));
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
  float density = sdl_window_ ? SDL_GetWindowPixelDensity(sdl_window_) : 1.0f;
  if (density <= 0.0f) {
    density = 1.0f;
  }
  WindowDestructionReceiver destruction_receiver(this);
  switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION: {
      if (GetCursorVisibility() == CursorVisibility::kAutoHidden) {
        SDL_ShowCursor();
        RearmCursorAutoHideTimer();
      }
      MouseEvent e(this, MouseEvent::Button::kNone, int32_t(event.motion.x * density),
                   int32_t(event.motion.y * density));
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
