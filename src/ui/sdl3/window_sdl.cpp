#include "ui/sdl3/window_sdl.h"

#include <cmath>
#include <cstring>
#include <filesystem>

#include <rex/cvar.h>
#include <rex/ui/video_mode.h>
#include <rex/fail.h>
#include <rex/logging/log.h>
#include <rex/platform.h>
#include "ui/sdl3/sdl_virtual_key.h"

#include "ui/sdl3/cvar_table.h"

#if REX_PLATFORM_WIN32
#include <rex/gpu/surface_win.h>
#elif REX_PLATFORM_MAC
#include <CoreFoundation/CoreFoundation.h>
#include <SDL3/SDL_metal.h>

#include <rex/gpu/surface_mac.h>
#else
#include <X11/Xlib-xcb.h>
#include <rex/gpu/surface_gnulinux.h>
#endif

namespace rex::ui {

namespace {

/**
 * SDL timer callback running on the SDL timer thread. Defers the hide to the UI thread, where it
 * touches only the global SDL cursor and the nonvirtual visibility getter. The window removes the
 * timer before destroying the SDL window.
 *
 * @param userdata The WindowSDL.
 * @param timer_id Unused.
 * @param interval Unused.
 * @return 0, which makes the timer one shot.
 */
Uint32 CursorAutoHideTimerCallback(void* userdata, SDL_TimerID timer_id, Uint32 interval) {
  (void)timer_id;
  (void)interval;
  auto* window = static_cast<WindowSDL*>(userdata);
  window->GetAppContext().CallInUIThreadDeferred([window] {
    if (window->GetCursorVisibility() == CursorVisibility::kAutoHidden) {
      SDL_HideCursor();
    }
  });
  return 0;
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
  u32 width = 0;
  u32 height = 0;
  ResolveConfiguredLogicalSize(width, height);
  return std::make_unique<WindowSDL>(app_context, title, width, height);
}

WindowSDL::WindowSDL(WindowedAppContext& app_context, const std::string_view title,
                     u32 desired_logical_width, u32 desired_logical_height)
    : Window(app_context, title, desired_logical_width, desired_logical_height) {}

WindowSDL::~WindowSDL() {
  EnterDestructor();
  DestroySDLWindow();
}

bool WindowSDL::OpenImpl() {
  // SDL window coordinates are physical pixels on Windows and X11, logical points on Cocoa.
  SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
#if REX_PLATFORM_MAC
  int initial_width = static_cast<int>(GetDesiredLogicalWidth());
  int initial_height = static_cast<int>(GetDesiredLogicalHeight());
#else
  int initial_width = static_cast<int>(SizeToPhysical(GetDesiredLogicalWidth()));
  int initial_height = static_cast<int>(SizeToPhysical(GetDesiredLogicalHeight()));
#endif
  sdl_window_ = SDL_CreateWindow(GetTitle().c_str(), initial_width, initial_height, flags);
  if (!sdl_window_) {
    REX_FAIL(ui, false, "SDL_CreateWindow failed: {}", SDL_GetError());
  }
  sdl_window_id_ = SDL_GetWindowID(sdl_window_);
  GetSDLAppContext().RegisterWindow(sdl_window_id_, this);
  ApplySizeLimits();

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
  // SDL3 requires opt in for text input events, reapplied so a reopened window keeps its state.
  ApplyTextInputActiveNow();
  ApplyCursorVisibilityNow();
  SDL_ShowWindow(sdl_window_);

  int pixel_width = 0;
  int pixel_height = 0;
  SDL_GetWindowSizeInPixels(sdl_window_, &pixel_width, &pixel_height);
  WindowDestructionReceiver destruction_receiver(this);
  OnActualSizeUpdate(static_cast<u32>(pixel_width), static_cast<u32>(pixel_height),
                     destruction_receiver);
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
#if REX_PLATFORM_MAC
  if (metal_view_) {
    SDL_Metal_DestroyView(static_cast<SDL_MetalView>(metal_view_));
    metal_view_ = nullptr;
  }
#endif
  if (cursor_hide_timer_) {
    SDL_RemoveTimer(cursor_hide_timer_);
    cursor_hide_timer_ = 0;
  }
  if (sdl_window_) {
    GetSDLAppContext().UnregisterWindow(sdl_window_id_);
    SDL_DestroyWindow(sdl_window_);
    sdl_window_ = nullptr;
    sdl_window_id_ = 0;
  }
}

void WindowSDL::Raise() {
  if (sdl_window_) {
    SDL_RaiseWindow(sdl_window_);
  }
}

NativeWindow WindowSDL::GetNative() {
  NativeWindow result;
  if (!sdl_window_) {
    return result;
  }
  result.sdl = sdl_window_;
  const SDL_PropertiesID props = SDL_GetWindowProperties(sdl_window_);
  if (void* hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)) {
    result.kind = NativeWindow::Kind::kWin32;
    result.window = hwnd;
    return result;
  }
  if (void* ns = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr)) {
    result.kind = NativeWindow::Kind::kCocoa;
    result.window = ns;
#if REX_PLATFORM_MAC
    result.layer = AcquireMetalLayer();
#endif
    return result;
  }
  if (void* surface =
          SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr)) {
    result.kind = NativeWindow::Kind::kWayland;
    result.window = surface;
    result.display =
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
    return result;
  }
  const u64 x11_window =
      static_cast<u64>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
  if (x11_window) {
    result.kind = NativeWindow::Kind::kX11;
    // NOLINTNEXTLINE(performance-no-int-to-ptr): NativeWindow carries the X11 XID as a pointer
    result.window = reinterpret_cast<void*>(x11_window);
    result.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
  }
  return result;
}

#if REX_PLATFORM_MAC
void* WindowSDL::AcquireMetalLayer() {
  if (!sdl_window_) {
    return nullptr;
  }
  if (!metal_view_) {
    metal_view_ = SDL_Metal_CreateView(sdl_window_);
  }
  return metal_view_ ? SDL_Metal_GetLayer(static_cast<SDL_MetalView>(metal_view_)) : nullptr;
}
#endif

SDL_Window* WindowSDL::GetSDL() const {
  return sdl_window_;
}

void WindowSDL::ApplySizeLimits() {
  if (!sdl_window_) {
    return;
  }
  SDL_SetWindowMinimumSize(sdl_window_, static_cast<i32>(min_size_.width_px),
                           static_cast<i32>(min_size_.height_px));
  f32 ratio = 0.0f;
  switch (aspect_ratio_) {
    case AspectRatio::kStretch:
      ratio = 0.0f;
      break;
    case AspectRatio::k4x3:
      ratio = 4.0f / 3.0f;
      break;
    case AspectRatio::k5x4:
      ratio = 5.0f / 4.0f;
      break;
    case AspectRatio::k3x2:
      ratio = 3.0f / 2.0f;
      break;
    case AspectRatio::k16x10:
      ratio = 16.0f / 10.0f;
      break;
    case AspectRatio::k16x9:
      ratio = 16.0f / 9.0f;
      break;
    case AspectRatio::k21x9:
      ratio = 21.0f / 9.0f;
      break;
    case AspectRatio::k32x9:
      ratio = 32.0f / 9.0f;
      break;
  }
  SDL_SetWindowAspectRatio(sdl_window_, ratio, ratio);
}

bool WindowSDL::SetRelativeMouseMode(bool enable) {
  if (!sdl_window_) {
    return false;
  }
  if (!SDL_SetWindowRelativeMouseMode(sdl_window_, enable)) {
    REXLOG_WARN(ui, "SDL_SetWindowRelativeMouseMode({}) failed: {}", enable, SDL_GetError());
    return false;
  }
  return enable;
}

bool WindowSDL::WarpMouseToCenter(i32& x_out, i32& y_out) {
  if (!sdl_window_) {
    return false;
  }
  int width = 0;
  int height = 0;
  SDL_GetWindowSize(sdl_window_, &width, &height);
  if (width <= 0 || height <= 0) {
    return false;
  }
  f32 center_x = static_cast<f32>(width) * 0.5f;
  f32 center_y = static_cast<f32>(height) * 0.5f;
  SDL_WarpMouseInWindow(sdl_window_, center_x, center_y);
  // The warp reports nothing back and compositors can drop it, so confirm.
  f32 actual_x = 0.0f;
  f32 actual_y = 0.0f;
  SDL_GetMouseState(&actual_x, &actual_y);
  if (std::fabs(actual_x - center_x) > 1.0f || std::fabs(actual_y - center_y) > 1.0f) {
    return false;
  }
  f32 density = GetPixelDensity();
  x_out = static_cast<i32>(center_x * density);
  y_out = static_cast<i32>(center_y * density);
  return true;
}

bool WindowSDL::GetDisplayPixelSize(u32& width, u32& height) const {
  if (!sdl_window_) {
    return false;
  }
  const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(sdl_window_));
  if (!mode) {
    return false;
  }
  width = static_cast<u32>(static_cast<f32>(mode->w) * mode->pixel_density);
  height = static_cast<u32>(static_cast<f32>(mode->h) * mode->pixel_density);
  return width > 0 && height > 0;
}

f32 WindowSDL::GetPixelDensity() const {
  f32 density = sdl_window_ ? SDL_GetWindowPixelDensity(sdl_window_) : 1.0f;
  return density > 0.0f ? density : 1.0f;
}

u32 WindowSDL::GetLatestDPIImpl() const {
  f32 scale = sdl_window_ ? SDL_GetWindowDisplayScale(sdl_window_)
                          : SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  if (scale <= 0.0f) {
    return GetMediumDPI();
  }
  return static_cast<u32>(std::lround(scale * static_cast<f32>(GetMediumDPI())));
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
  SDL_SetWindowSize(sdl_window_, static_cast<int>(GetDesiredLogicalWidth()),
                    static_cast<int>(GetDesiredLogicalHeight()));
#else
  SDL_SetWindowSize(sdl_window_, static_cast<int>(SizeToPhysical(GetDesiredLogicalWidth())),
                    static_cast<int>(SizeToPhysical(GetDesiredLogicalHeight())));
#endif
}

void WindowSDL::CenterOnConfiguredDisplay() {
  const i32 monitor_index = GetMonitor();
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
    REXLOG_WARN(ui, "monitor cvar is {} but only {} display(s) present, using default",
                monitor_index, display_count);
  }
  SDL_free(displays);
}

void WindowSDL::ApplyFullscreenModeNow() {
  if (!sdl_window_) {
    return;
  }
  if (!REXCVAR_GET(::rex::ui::cvars::fullscreen_exclusive)) {
    SDL_SetWindowFullscreenMode(sdl_window_, nullptr);
    return;
  }

  const SDL_DisplayID display = SDL_GetDisplayForWindow(sdl_window_);
  i32 width = 0;
  i32 height = 0;
  if (!TryParseResolutionPreset(REXCVAR_GET(::rex::ui::cvars::resolution), width, height) ||
      width <= 0 || height <= 0) {
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    if (!desktop) {
      REXLOG_WARN(ui, "No desktop mode for display {}: staying borderless",
                  static_cast<u32>(display));
      SDL_SetWindowFullscreenMode(sdl_window_, nullptr);
      return;
    }
    width = desktop->w;
    height = desktop->h;
  }

  SDL_DisplayMode mode = {};
  if (!SDL_GetClosestFullscreenDisplayMode(display, width, height, 0.0f, true, &mode)) {
    REXLOG_WARN(ui, "Display {} has no mode near {}x{}: staying borderless",
                static_cast<u32>(display), width, height);
    SDL_SetWindowFullscreenMode(sdl_window_, nullptr);
    return;
  }
  if (!SDL_SetWindowFullscreenMode(sdl_window_, &mode)) {
    REXLOG_WARN(ui, "SDL_SetWindowFullscreenMode({}x{}) failed: {}", mode.w, mode.h,
                SDL_GetError());
    return;
  }
  REXLOG_INFO(ui, "Exclusive fullscreen mode {}x{} @ {:.3g}Hz on display {}", mode.w, mode.h,
              mode.refresh_rate, static_cast<u32>(display));
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
      // AutoHidden hides at once, mouse motion reveals the cursor and re-arms the timer.
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

std::unique_ptr<gpu::Surface> WindowSDL::CreateSurfaceImpl(gpu::Surface::TypeFlags allowed_types) {
  if (!sdl_window_) {
    return nullptr;
  }
#if REX_PLATFORM_WIN32
  SDL_PropertiesID props = SDL_GetWindowProperties(sdl_window_);
  if (allowed_types & gpu::Surface::kTypeFlagWin32HWND) {
    HWND hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    HINSTANCE hinstance = static_cast<HINSTANCE>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr));
    if (hwnd) {
      return std::make_unique<gpu::Win32HWNDSurface>(hinstance, hwnd);
    }
  }
#elif REX_PLATFORM_MAC
  if (allowed_types & gpu::Surface::kTypeFlagCAMetalLayer) {
    if (void* layer = AcquireMetalLayer()) {
      return std::make_unique<gpu::CAMetalLayerSurface>(sdl_window_, layer);
    }
  }
#else
  SDL_PropertiesID props = SDL_GetWindowProperties(sdl_window_);
  if (allowed_types & gpu::Surface::kTypeFlagWaylandSurface) {
    auto* wl_display_ptr = static_cast<struct wl_display*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
    auto* wl_surface_ptr = static_cast<struct wl_surface*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
    if (wl_display_ptr && wl_surface_ptr) {
      return std::make_unique<gpu::WaylandSurface>(wl_display_ptr, wl_surface_ptr, sdl_window_);
    }
  }
  if (allowed_types & gpu::Surface::kTypeFlagXCBWindow) {
    auto* display = static_cast<Display*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
    auto x11_window = static_cast<xcb_window_t>(
        SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    if (display && x11_window) {
      return std::make_unique<gpu::XCBWindowSurface>(XGetXCBConnection(display), x11_window);
    }
  }
#endif
  return nullptr;
}

void WindowSDL::RequestPaintImpl() {
  // At most 1 paint event is queued at a time, SDL_PushEvent is thread-safe.
  if (paint_pending_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  SDL_Event event{};
  event.type = GetSDLAppContext().GetPaintEventType();
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
      OnActualSizeUpdate(static_cast<u32>(event.window.data1), static_cast<u32>(event.window.data2),
                         destruction_receiver);
      break;
    case SDL_EVENT_WINDOW_RESIZED: {
      SDL_WindowFlags flags = SDL_GetWindowFlags(sdl_window_);
      if (!(flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED))) {
#if REX_PLATFORM_MAC
        // Cocoa reports logical points, converting again would halve the size on Retina displays.
        OnDesiredLogicalSizeUpdate(static_cast<u32>(event.window.data1),
                                   static_cast<u32>(event.window.data2));
#else
        OnDesiredLogicalSizeUpdate(SizeToLogical(static_cast<u32>(event.window.data1)),
                                   SizeToLogical(static_cast<u32>(event.window.data2)));
#endif
      }
      break;
    }
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
      UISetupEvent e(UIEvent::Type::kDPIChanged, this);
      OnDPIChanged(e, destruction_receiver);
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
      // The platform cannot retain the previous image, so force the paint.
      OnPaint(true);
      break;
    case SDL_EVENT_WINDOW_MINIMIZED:
      OnMinimized(destruction_receiver);
      break;
    case SDL_EVENT_WINDOW_RESTORED:
      OnRestored(destruction_receiver);
      break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
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
  hid::VirtualKey virtual_key = TranslateSDLScancode(event.key.scancode);
  if (virtual_key == hid::VirtualKey::kNone) {
    return;
  }
  SDL_Keymod mod = event.key.mod;
  KeyEvent e(event.type == SDL_EVENT_KEY_DOWN ? UIEvent::Type::kKeyDown : UIEvent::Type::kKeyUp,
             this, virtual_key, /*repeat_count=*/1,
             /*prev_state=*/event.key.repeat,
             /*modifier_shift_pressed=*/(mod & SDL_KMOD_SHIFT) != 0,
             /*modifier_ctrl_pressed=*/(mod & SDL_KMOD_CTRL) != 0,
             /*modifier_alt_pressed=*/(mod & SDL_KMOD_ALT) != 0);
  WindowDestructionReceiver destruction_receiver(this);
  if (event.type == SDL_EVENT_KEY_DOWN) {
    OnKeyDown(e, destruction_receiver);
  } else {
    OnKeyUp(e, destruction_receiver);
  }
}

void WindowSDL::HandleTextInputEvent(SDL_Event& event) {
  // One OnKeyChar per codepoint with the character code in the virtual key slot.
  const char* text = event.text.text;
  if (!text || !*text) {
    return;
  }
  WindowDestructionReceiver destruction_receiver(this);
  const char* it = text;
  size_t remaining = std::strlen(text);
  while (remaining > 0) {
    const u32 codepoint = SDL_StepUTF8(&it, &remaining);
    KeyEvent e(UIEvent::Type::kChar, this, static_cast<hid::VirtualKey>(codepoint), 1, false, false,
               false, false);
    OnKeyChar(e, destruction_receiver);
    if (destruction_receiver.IsWindowDestroyed()) {
      return;
    }
  }
}

void WindowSDL::HandleMouseEvent(SDL_Event& event) {
  // SDL3 reports float window coordinates, listeners expect physical pixels.
  f32 density = GetPixelDensity();
  WindowDestructionReceiver destruction_receiver(this);
  switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION: {
      if (GetCursorVisibility() == CursorVisibility::kAutoHidden) {
        SDL_ShowCursor();
        RearmCursorAutoHideTimer();
      }
      MouseEvent e(UIEvent::Type::kMouseMove, this, MouseEvent::Button::kNone,
                   static_cast<i32>(event.motion.x * density),
                   static_cast<i32>(event.motion.y * density), 0, 0, event.motion.xrel * density,
                   event.motion.yrel * density);
      OnMouseMove(e, destruction_receiver);
      break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      MouseEvent e(event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? UIEvent::Type::kMouseDown
                                                             : UIEvent::Type::kMouseUp,
                   this, TranslateSDLMouseButton(event.button.button),
                   static_cast<i32>(event.button.x * density),
                   static_cast<i32>(event.button.y * density));
      if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        OnMouseDown(e, destruction_receiver);
      } else {
        OnMouseUp(e, destruction_receiver);
      }
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      MouseEvent e(
          UIEvent::Type::kMouseWheel, this, MouseEvent::Button::kNone,
          static_cast<i32>(event.wheel.mouse_x * density),
          static_cast<i32>(event.wheel.mouse_y * density),
          static_cast<i32>(event.wheel.x * static_cast<f32>(MouseEvent::kScrollPerDetent)),
          static_cast<i32>(event.wheel.y * static_cast<f32>(MouseEvent::kScrollPerDetent)));
      OnMouseWheel(e, destruction_receiver);
      break;
    }
    default:
      break;
  }
}

}  // namespace rex::ui
