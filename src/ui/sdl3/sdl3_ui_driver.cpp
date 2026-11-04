#include "ui/sdl3/sdl3_ui_driver.h"

#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <thread>

#include <SDL3/SDL.h>

#include <rex/logging/log_category.h>
#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/ui/native_window.h>
#include <rex/ui/ui_driver.h>
#include <rex/ui/dialog.h>
#include <rex/ui/window.h>
#include <rex/ui/ui_event.h>
#include <rex/platform.h>
#include <rex/settings_registry.h>

#include "ui/sdl3/message_box.h"
#include "ui/sdl3/sdl_helper.h"
#include "ui/sdl3/sdl_virtual_key.h"
#include "ui/sdl3/video_scope.h"

#if REX_PLATFORM_MAC
#include <CoreFoundation/CoreFoundation.h>
#include <SDL3/SDL_metal.h>
#endif

namespace rex::ui::sdl3 {

namespace {

std::unique_ptr<IDriver> CreateDriver() {
  return std::make_unique<SDL3UIDriver>();
}

constexpr std::array<DriverRegistration, 1> kDrivers = {
    DriverRegistration{DriverType::kUI, "sdl3", {}, &CreateDriver}};

/** Wheel units per detent that the pointer reports, matching the Windows WHEEL_DELTA. */
constexpr i32 kScrollPerDetent = 120;

hid::MouseButton TranslateButton(Uint8 button) {
  switch (button) {
    case SDL_BUTTON_LEFT:
      return hid::MouseButton::kLeft;
    case SDL_BUTTON_RIGHT:
      return hid::MouseButton::kRight;
    case SDL_BUTTON_MIDDLE:
      return hid::MouseButton::kMiddle;
    case SDL_BUTTON_X1:
      return hid::MouseButton::kX1;
    case SDL_BUTTON_X2:
      return hid::MouseButton::kX2;
    default:
      return hid::MouseButton::kNone;
  }
}

hid::KeyModifiers ToModifiers(SDL_Keymod mod) {
  hid::KeyModifiers modifiers = hid::KeyModifiers::kNone;
  if ((mod & SDL_KMOD_SHIFT) != 0) {
    modifiers |= hid::KeyModifiers::kShift;
  }
  if ((mod & SDL_KMOD_CTRL) != 0) {
    modifiers |= hid::KeyModifiers::kCtrl;
  }
  if ((mod & SDL_KMOD_ALT) != 0) {
    modifiers |= hid::KeyModifiers::kAlt;
  }
  return modifiers;
}

f32 ToDetents(f32 turns) {
  return static_cast<f32>(static_cast<i32>(turns * static_cast<f32>(kScrollPerDetent))) /
         static_cast<f32>(kScrollPerDetent);
}

f32 RatioOf(AspectRatio ratio) {
  switch (ratio) {
    case AspectRatio::kStretch:
      return 0.0f;
    case AspectRatio::k4x3:
      return 4.0f / 3.0f;
    case AspectRatio::k5x4:
      return 5.0f / 4.0f;
    case AspectRatio::k3x2:
      return 3.0f / 2.0f;
    case AspectRatio::k16x10:
      return 16.0f / 10.0f;
    case AspectRatio::k16x9:
      return 16.0f / 9.0f;
    case AspectRatio::k21x9:
      return 21.0f / 9.0f;
    case AspectRatio::k32x9:
      return 32.0f / 9.0f;
  }
  return 0.0f;
}

SDL_SystemCursor ToSystemCursor(CursorShape shape) {
  switch (shape) {
    case CursorShape::kArrow:
      return SDL_SYSTEM_CURSOR_DEFAULT;
    case CursorShape::kTextInput:
      return SDL_SYSTEM_CURSOR_TEXT;
    case CursorShape::kResizeAll:
      return SDL_SYSTEM_CURSOR_MOVE;
    case CursorShape::kResizeNS:
      return SDL_SYSTEM_CURSOR_NS_RESIZE;
    case CursorShape::kResizeEW:
      return SDL_SYSTEM_CURSOR_EW_RESIZE;
    case CursorShape::kResizeNESW:
      return SDL_SYSTEM_CURSOR_NESW_RESIZE;
    case CursorShape::kResizeNWSE:
      return SDL_SYSTEM_CURSOR_NWSE_RESIZE;
    case CursorShape::kHand:
      return SDL_SYSTEM_CURSOR_POINTER;
    case CursorShape::kNotAllowed:
      return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
  }
  return SDL_SYSTEM_CURSOR_DEFAULT;
}

}  // namespace

struct SDL3UIDriver::State {
  IDriverContext* context = nullptr;
  bool video_started = false;
  SDL_Window* window = nullptr;
  SDL_WindowID window_id = 0;
  u32 wakeup_event = 0;
  bool event_watch = false;
  bool fullscreen = false;
  bool exclusive = false;
  i32 mode_width = 0;
  i32 mode_height = 0;
  i32 monitor = 0;
  bool has_focus = false;
  bool text_input = false;
  bool quit_requested = false;
  hid::PointerMode pointer_mode = hid::PointerMode::kFree;
  bool relative = false;
  std::array<SDL_Cursor*, 9> cursors{};
  std::thread::id main_thread;
  IUIEventListener* listener = nullptr;
#if REX_PLATFORM_MAC
  void* metal_view = nullptr;
#endif

  f32 GetPixelDensity() const {
    const f32 density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
  }

  void CenterOnMonitor() {
    if (!window || monitor <= 0) {
      return;
    }
    int display_count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&display_count);
    if (!displays) {
      return;
    }
    if (monitor <= display_count) {
      const SDL_DisplayID display = displays[monitor - 1];
      SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                            SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    } else {
      REXUI_WARN("monitor is {} but only {} display(s) present, using default", monitor,
                 display_count);
    }
    SDL_free(displays);
  }

  void ApplyFullscreenMode() {
    if (!window) {
      return;
    }
    if (!exclusive) {
      SDL_SetWindowFullscreenMode(window, nullptr);
      return;
    }
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    i32 width = mode_width;
    i32 height = mode_height;
    if (width <= 0 || height <= 0) {
      const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
      if (!desktop) {
        REXUI_WARN("No desktop mode for display {}, staying borderless", static_cast<u32>(display));
        SDL_SetWindowFullscreenMode(window, nullptr);
        return;
      }
      width = desktop->w;
      height = desktop->h;
    }
    SDL_DisplayMode mode = {};
    if (!SDL_GetClosestFullscreenDisplayMode(display, width, height, 0.0f, true, &mode)) {
      REXUI_WARN("Display {} has no mode near {}x{}, staying borderless", static_cast<u32>(display),
                 width, height);
      SDL_SetWindowFullscreenMode(window, nullptr);
      return;
    }
    if (!SDL_SetWindowFullscreenMode(window, &mode)) {
      REXUI_WARN("SDL_SetWindowFullscreenMode({}x{}) failed: {}", mode.w, mode.h, SDL_GetError());
      return;
    }
    REXUI_INFO("Exclusive fullscreen mode {}x{} @ {:.3g}Hz on display {}", mode.w, mode.h,
               mode.refresh_rate, static_cast<u32>(display));
  }

  void Deliver(const UIEvent& event) {
    if (listener) {
      listener->OnUIEvent(event);
    }
  }

  void Deliver(hid::InputEvent& event) {
    if (listener) {
      listener->OnInputEvent(event);
    }
  }

  void HandleWindowEvent(const SDL_Event& event) {
    switch (event.type) {
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
        UISetupEvent e(UIEvent::Type::kResize);
        Deliver(e);
        break;
      }
      case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
        UISetupEvent e(UIEvent::Type::kDPIChanged);
        Deliver(e);
        break;
      }
      case SDL_EVENT_WINDOW_DISPLAY_CHANGED: {
        MonitorUpdateEvent e(true);
        Deliver(e);
        break;
      }
      case SDL_EVENT_WINDOW_FOCUS_GAINED:
      case SDL_EVENT_WINDOW_FOCUS_LOST: {
        has_focus = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        UISetupEvent e(UIEvent::Type::kFocus);
        Deliver(e);
        break;
      }
      case SDL_EVENT_WINDOW_MINIMIZED: {
        UIEvent e(UIEvent::Type::kMinimized);
        Deliver(e);
        break;
      }
      case SDL_EVENT_WINDOW_RESTORED: {
        UIEvent e(UIEvent::Type::kRestored);
        Deliver(e);
        break;
      }
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED: {
        UIEvent e(UIEvent::Type::kClose);
        Deliver(e);
        break;
      }
      default:
        break;
    }
  }

  void HandleKey(const SDL_Event& event) {
    const hid::VirtualKey virtual_key = TranslateSDLScancode(event.key.scancode);
    if (virtual_key == hid::VirtualKey::kNone) {
      return;
    }
    hid::InputEvent e{.type = event.type == SDL_EVENT_KEY_DOWN ? hid::InputEvent::Type::kKeyDown
                                                               : hid::InputEvent::Type::kKeyUp,
                      .key = virtual_key,
                      .modifiers = ToModifiers(event.key.mod),
                      .repeat = event.key.repeat};
    Deliver(e);
  }

  void HandleText(const SDL_Event& event) {
    const char* text = event.text.text;
    if (!text || !*text) {
      return;
    }
    const char* it = text;
    size_t remaining = std::strlen(text);
    while (remaining > 0) {
      const u32 codepoint = SDL_StepUTF8(&it, &remaining);
      hid::InputEvent e{.type = hid::InputEvent::Type::kKeyChar, .character = codepoint};
      Deliver(e);
    }
  }

  void HandleMouse(const SDL_Event& event) {
    const f32 density = GetPixelDensity();
    switch (event.type) {
      case SDL_EVENT_MOUSE_MOTION: {
        f32 dx = event.motion.xrel * density;
        f32 dy = event.motion.yrel * density;
        const f32 x = event.motion.x * density;
        const f32 y = event.motion.y * density;
        if (pointer_mode == hid::PointerMode::kLocked && !relative && window) {
          int width = 0;
          int height = 0;
          SDL_GetWindowSize(window, &width, &height);
          const f32 center_x = static_cast<f32>(width) * 0.5f;
          const f32 center_y = static_cast<f32>(height) * 0.5f;
          dx = (event.motion.x - center_x) * density;
          dy = (event.motion.y - center_y) * density;
          if (std::fabs(dx) > 0.0f || std::fabs(dy) > 0.0f) {
            SDL_WarpMouseInWindow(window, center_x, center_y);
          }
        }
        hid::InputEvent e{.type = hid::InputEvent::Type::kMotion,
                          .x = std::trunc(x),
                          .y = std::trunc(y),
                          .dx = dx,
                          .dy = dy};
        Deliver(e);
        break;
      }
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP: {
        hid::InputEvent e{.type = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                                      ? hid::InputEvent::Type::kButtonDown
                                      : hid::InputEvent::Type::kButtonUp,
                          .button = TranslateButton(event.button.button),
                          .x = std::trunc(event.button.x * density),
                          .y = std::trunc(event.button.y * density)};
        Deliver(e);
        break;
      }
      case SDL_EVENT_MOUSE_WHEEL: {
        hid::InputEvent e{.type = hid::InputEvent::Type::kWheel,
                          .x = std::trunc(event.wheel.mouse_x * density),
                          .y = std::trunc(event.wheel.mouse_y * density),
                          .scroll_x = ToDetents(event.wheel.x),
                          .scroll_y = ToDetents(event.wheel.y)};
        Deliver(e);
        break;
      }
      default:
        break;
    }
  }

  void Process(const SDL_Event& event) {
    if (event.type == wakeup_event) {
      return;
    }
    if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) {
      if (event.window.windowID == window_id) {
        HandleWindowEvent(event);
      }
      return;
    }
    switch (event.type) {
      case SDL_EVENT_QUIT: {
        quit_requested = true;
        UIEvent e(UIEvent::Type::kClose);
        Deliver(e);
        break;
      }
      case SDL_EVENT_KEY_DOWN:
      case SDL_EVENT_KEY_UP:
        if (event.key.windowID == window_id) {
          HandleKey(event);
        }
        break;
      case SDL_EVENT_TEXT_INPUT:
        if (event.text.windowID == window_id) {
          HandleText(event);
        }
        break;
      case SDL_EVENT_MOUSE_MOTION:
        if (event.motion.windowID == window_id) {
          HandleMouse(event);
        }
        break;
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.windowID == window_id) {
          HandleMouse(event);
        }
        break;
      case SDL_EVENT_MOUSE_WHEEL:
        if (event.wheel.windowID == window_id) {
          HandleMouse(event);
        }
        break;
      case SDL_EVENT_DROP_FILE:
        if (event.drop.windowID == window_id) {
          FileDropEvent e(std::filesystem::path(event.drop.data));
          Deliver(e);
        }
        break;
      default:
        break;
    }
  }
};

SDL3UIDriver::SDL3UIDriver() : SettingOwner("ui.sdl"), state_(std::make_unique<State>()) {}

SDL3UIDriver::~SDL3UIDriver() {
  Shutdown();
}

std::string_view SDL3UIDriver::GetName() const {
  return "sdl3";
}

Result<void> SDL3UIDriver::Initialize(IDriverContext& context) {
  State& s = *state_;
  s.context = &context;
  s.main_thread = std::this_thread::get_id();
  REX_TRY_VOID(context.GetSettingsRegistry().Register(*this));
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  std::string driver = video_driver.Get();
#if REX_PLATFORM_MAC
  if (driver.empty()) {
    driver = "cocoa";
  }
#endif
  if (!driver.empty()) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, driver.c_str());
  }
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    REXUI_WARN("no video driver, running headless: {}", SDL_GetError());
    return Ok();
  }
  s.video_started = true;
  const char* in_use = SDL_GetCurrentVideoDriver();
  REXUI_INFO("SDL video driver: {}", in_use ? in_use : "unknown");
  s.wakeup_event = SDL_RegisterEvents(1);
  if (s.wakeup_event == 0) {
    Shutdown();
    return Err(ErrorCategory::kPlatform,
               std::format("SDL_RegisterEvents failed: {}", SDL_GetError()));
  }
  return Ok();
}

void SDL3UIDriver::Shutdown() {
  State& s = *state_;
  CloseUIWindow();
  for (SDL_Cursor*& cursor : s.cursors) {
    if (cursor) {
      SDL_DestroyCursor(cursor);
      cursor = nullptr;
    }
  }
  if (s.video_started) {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    s.video_started = false;
  }
  if (s.context) {
    s.context->GetSettingsRegistry().Unregister(*this);
    s.context = nullptr;
  }
}

bool SDL3UIDriver::HasDisplay() const {
  const State& s = *state_;
  if (!s.video_started) {
    return false;
  }
  const char* driver = SDL_GetCurrentVideoDriver();
  return driver && std::strcmp(driver, "dummy") != 0;
}

Result<void> SDL3UIDriver::OpenWindow(const WindowConfig& config) {
  State& s = *state_;
  if (s.window) {
    return Err(ErrorCategory::kRuntime, "a window is already open");
  }
  SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
#if REX_PLATFORM_LINUX
  if (config.vulkan_surface) {
    flags |= SDL_WINDOW_VULKAN;
  }
#endif
#if REX_PLATFORM_MAC
  const int width = static_cast<int>(config.width);
  const int height = static_cast<int>(config.height);
#else
  const int width = static_cast<int>(config.width);
  const int height = static_cast<int>(config.height);
#endif
  s.window = SDL_CreateWindow(config.title.c_str(), width, height, flags);
  if (!s.window) {
    return Err(ErrorCategory::kPlatform,
               std::format("SDL_CreateWindow failed: {}", SDL_GetError()));
  }
  s.window_id = SDL_GetWindowID(s.window);
  s.fullscreen = config.fullscreen;
  s.exclusive = config.fullscreen_exclusive;
  s.mode_width = static_cast<i32>(config.mode_width);
  s.mode_height = static_cast<i32>(config.mode_height);
  s.monitor = config.monitor;
  SetLimits(config.min_width, config.min_height, config.aspect_ratio);
  s.CenterOnMonitor();
  if (s.fullscreen) {
    s.ApplyFullscreenMode();
    SDL_SetWindowFullscreen(s.window, true);
  }
#if REX_PLATFORM_MAC
  CFPreferencesSetAppValue(CFSTR("ApplePressAndHoldEnabled"), kCFBooleanFalse,
                           kCFPreferencesCurrentApplication);
  CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication);
#endif
  SDL_ShowWindow(s.window);
  s.has_focus = (SDL_GetWindowFlags(s.window) & SDL_WINDOW_INPUT_FOCUS) != 0;
  return Ok();
}

void SDL3UIDriver::CloseUIWindow() {
  State& s = *state_;
#if REX_PLATFORM_MAC
  if (s.metal_view) {
    SDL_Metal_DestroyView(static_cast<SDL_MetalView>(s.metal_view));
    s.metal_view = nullptr;
  }
#endif
  if (s.window) {
    SDL_DestroyWindow(s.window);
    s.window = nullptr;
    s.window_id = 0;
    s.has_focus = false;
  }
  s.relative = false;
  s.pointer_mode = hid::PointerMode::kFree;
}

void SDL3UIDriver::SetTitle(std::string_view title) {
  State& s = *state_;
  if (s.window) {
    SDL_SetWindowTitle(s.window, std::string(title).c_str());
  }
}

void SDL3UIDriver::SetSize(u32 width, u32 height) {
  State& s = *state_;
  if (!s.window || (SDL_GetWindowFlags(s.window) &
                    (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED))) {
    return;
  }
  SDL_SetWindowSize(s.window, static_cast<int>(width), static_cast<int>(height));
}

void SDL3UIDriver::SetLimits(u32 min_width, u32 min_height, AspectRatio ratio) {
  State& s = *state_;
  if (!s.window) {
    return;
  }
  SDL_SetWindowMinimumSize(s.window, static_cast<int>(min_width), static_cast<int>(min_height));
  const f32 value = RatioOf(ratio);
  SDL_SetWindowAspectRatio(s.window, value, value);
}

void SDL3UIDriver::SetFullscreen(bool fullscreen, bool exclusive, i32 monitor, u32 mode_width,
                                 u32 mode_height) {
  State& s = *state_;
  if (!s.window) {
    return;
  }
  const bool monitor_changed = monitor != s.monitor;
  s.fullscreen = fullscreen;
  s.exclusive = exclusive;
  s.mode_width = static_cast<i32>(mode_width);
  s.mode_height = static_cast<i32>(mode_height);
  s.monitor = monitor;
  if (monitor_changed) {
    const bool was_fullscreen = (SDL_GetWindowFlags(s.window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (was_fullscreen) {
      SDL_SetWindowFullscreen(s.window, false);
      SDL_SyncWindow(s.window);
    }
    s.CenterOnMonitor();
  }
  s.ApplyFullscreenMode();
  SDL_SetWindowFullscreen(s.window, fullscreen);
}

void SDL3UIDriver::RaiseWindow() {
  State& s = *state_;
  if (s.window) {
    SDL_RaiseWindow(s.window);
  }
}

void SDL3UIDriver::SetCursorShape(bool visible, CursorShape shape) {
  State& s = *state_;
  const size_t index = static_cast<size_t>(shape);
  if (index < s.cursors.size()) {
    if (!s.cursors[index]) {
      s.cursors[index] = SDL_CreateSystemCursor(ToSystemCursor(shape));
    }
    if (s.cursors[index]) {
      SDL_SetCursor(s.cursors[index]);
    }
  }
  if (visible) {
    SDL_ShowCursor();
  } else {
    SDL_HideCursor();
  }
}

void SDL3UIDriver::SetPointerMode(hid::PointerMode mode) {
  State& s = *state_;
  if (s.pointer_mode == hid::PointerMode::kLocked) {
    if (s.window && s.relative) {
      SDL_SetWindowRelativeMouseMode(s.window, false);
    }
    SDL_CaptureMouse(false);
    s.relative = false;
  }
  s.pointer_mode = mode;
  if (mode != hid::PointerMode::kLocked || !s.window) {
    return;
  }
  SDL_CaptureMouse(true);
  if (SDL_SetWindowRelativeMouseMode(s.window, true)) {
    s.relative = true;
    return;
  }
  REXUI_WARN("pointer lock is unavailable, mouse look recenters the pointer instead, {}",
             SDL_GetError());
  int width = 0;
  int height = 0;
  SDL_GetWindowSize(s.window, &width, &height);
  SDL_WarpMouseInWindow(s.window, static_cast<f32>(width) * 0.5f, static_cast<f32>(height) * 0.5f);
}

void SDL3UIDriver::CaptureMouse(bool captured) {
  SDL_CaptureMouse(captured);
}

f32 SDL3UIDriver::GetContentScale() const {
  const State& s = *state_;
  const f32 density = s.GetPixelDensity();
  f32 scale = s.window ? SDL_GetWindowDisplayScale(s.window)
                       : SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  if (scale <= 0.0f) {
    scale = 1.0f;
  }
#if REX_PLATFORM_MAC
  return density;
#else
  (void)density;
  return scale;
#endif
}

WindowSize SDL3UIDriver::GetWindowSize() const {
  const State& s = *state_;
  if (!s.window) {
    return {};
  }
  int width = 0;
  int height = 0;
  SDL_GetWindowSizeInPixels(s.window, &width, &height);
  return {static_cast<u32>(std::max(width, 0)), static_cast<u32>(std::max(height, 0))};
}

bool SDL3UIDriver::HasFocus() const {
  return state_->has_focus;
}

void SDL3UIDriver::SetTextInputActive(bool active) {
  State& s = *state_;
  s.text_input = active;
  if (!s.window) {
    return;
  }
  if (active) {
    SDL_StartTextInput(s.window);
  } else {
    SDL_StopTextInput(s.window);
  }
}

Result<NativeWindow> SDL3UIDriver::GetNativeWindow() const {
  const State& s = *state_;
  if (!s.window) {
    return Err<NativeWindow>(ErrorCategory::kNotFound, "the window is not open");
  }
  NativeWindow result;
  result.sdl = s.window;
  const SDL_PropertiesID props = SDL_GetWindowProperties(s.window);
  if (void* hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)) {
    result.type = NativeWindow::Type::kWin32;
    result.window = hwnd;
    return result;
  }
  if (void* ns = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr)) {
    result.type = NativeWindow::Type::kCocoa;
    result.window = ns;
#if REX_PLATFORM_MAC
    State& mutable_state = *state_;
    if (!mutable_state.metal_view) {
      mutable_state.metal_view = SDL_Metal_CreateView(s.window);
    }
    result.layer = mutable_state.metal_view
                       ? SDL_Metal_GetLayer(static_cast<SDL_MetalView>(mutable_state.metal_view))
                       : nullptr;
#endif
    return result;
  }
  if (void* surface =
          SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr)) {
    result.type = NativeWindow::Type::kWayland;
    result.window = surface;
    result.display =
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
    return result;
  }
  const u64 x11_window =
      static_cast<u64>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
  if (x11_window) {
    result.type = NativeWindow::Type::kX11;
    // NOLINTNEXTLINE(performance-no-int-to-ptr): NativeWindow carries the X11 XID as a pointer
    result.window = reinterpret_cast<void*>(x11_window);
    result.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
  }
  return result;
}

bool SDL3UIDriver::WaitEvents(IUIEventListener& listener, u32 timeout_ms) {
  State& s = *state_;
  s.listener = &listener;
  SDL_Event event;
  const bool got = timeout_ms > 0 ? SDL_WaitEventTimeout(&event, static_cast<Sint32>(timeout_ms))
                                  : SDL_PollEvent(&event);
  if (got) {
    s.Process(event);
    while (SDL_PollEvent(&event)) {
      s.Process(event);
    }
  }
  s.listener = nullptr;
  return !s.quit_requested;
}

void SDL3UIDriver::WakeUp() {
  const State& s = *state_;
  if (s.wakeup_event == 0) {
    return;
  }
  SDL_Event event{};
  event.type = s.wakeup_event;
  SDL_PushEvent(&event);
}

Result<u32> SDL3UIDriver::ShowMessageBox(MessageType type, std::string_view title,
                                         std::string_view body,
                                         std::span<const std::string> buttons) {
  State& s = *state_;
  VideoScope video;
  if (!video.IsOk()) {
    REXUI_WARN("no usable message box driver, {}", video.GetError());
    return Err<u32>(ErrorCategory::kPlatform, video.GetError());
  }
  const bool on_main = std::this_thread::get_id() == s.main_thread;
  SDL_Window* parent = on_main ? s.window : nullptr;
  return ShowNativeBox(type, std::string(title), std::string(body), buttons, parent);
}

Result<std::vector<std::filesystem::path>> SDL3UIDriver::ShowFileDialog(
    const FileDialogConfig& config) {
  State& s = *state_;
  const bool on_main = std::this_thread::get_id() == s.main_thread;
  return ShowSDLFileDialog(config, on_main ? s.window : nullptr);
}

std::vector<Monitor> SDL3UIDriver::GetDisplays() {
  return GetSDLDisplays();
}

std::vector<HostLocale> SDL3UIDriver::GetPreferredLocales() {
  return GetSDLPreferredLocales();
}

std::string SDL3UIDriver::GetClipboardText() {
  char* text = SDL_GetClipboardText();
  std::string result = text ? text : "";
  SDL_free(text);
  return result;
}

void SDL3UIDriver::SetClipboardText(std::string_view text) {
  SDL_SetClipboardText(std::string(text).c_str());
}

std::string_view SDL3UIProvider::GetName() const {
  return "rexui-sdl3";
}

Result<void> SDL3UIProvider::Connect(IInterfaceFactory&) {
  rex::memory::SetTagName(rex::ui::kUI, "UI");
  return Ok();
}

void SDL3UIProvider::Disconnect() {}

Result<void> SDL3UIProvider::Init() {
  return Ok();
}

void SDL3UIProvider::Shutdown() {}

std::span<const DriverRegistration> SDL3UIProvider::GetDrivers() const {
  return kDrivers;
}

}  // namespace rex::ui::sdl3

REX_DECLARE_MODULE(rex::ui::sdl3::SDL3UIProvider)
