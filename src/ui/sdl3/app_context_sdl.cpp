#include "ui/sdl3/app_context_sdl.h"

#include <cstdlib>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/fail.h>
#include <rex/logging/log.h>
#include <rex/platform.h>
#include "ui/sdl3/window_sdl.h"

namespace rex::ui {

SDL3WindowedAppContext::~SDL3WindowedAppContext() {
  if (event_watch_registered_) {
    SDL_RemoveEventWatch(WatchEvent, this);
  }
  ExecutePendingFunctionsFromUIThread();
  if (SDL_WasInit(SDL_INIT_VIDEO)) {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  }
}

bool SDL3WindowedAppContext::Initialize(const UIConfig& config) {
  std::string requested_driver = config.video_driver;
#if REX_PLATFORM_MAC
  // macOS presents through a CAMetalLayer surface from the Cocoa driver.
  if (requested_driver.empty()) {
    requested_driver = "cocoa";
  }
#endif
  if (!requested_driver.empty()) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, requested_driver.c_str());
  }
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    REX_FAIL(ui, false, "SDL_InitSubSystem(SDL_INIT_VIDEO) failed: {}", SDL_GetError());
  }
  const char* video_driver_in_use = SDL_GetCurrentVideoDriver();
  REXLOG_INFO(ui, "SDL video driver: {}", video_driver_in_use ? video_driver_in_use : "unknown");
  u32 first = SDL_RegisterEvents(2);
  if (first == 0) {
    REX_FAIL(ui, false, "SDL_RegisterEvents failed: {}", SDL_GetError());
  }
  wakeup_event_type_ = first;
  paint_event_type_ = first + 1;
  if (!SDL_AddEventWatch(WatchEvent, this)) {
    REX_FAIL(ui, false, "SDL_AddEventWatch failed: {}", SDL_GetError());
  }
  event_watch_registered_ = true;
  return true;
}

void SDL3WindowedAppContext::NotifyUILoopOfPendingFunctions() {
  // SDL_PushEvent is thread-safe by SDL contract.
  SDL_Event event{};
  event.type = wakeup_event_type_;
  SDL_PushEvent(&event);
}

void SDL3WindowedAppContext::PlatformQuitFromUIThread() {
  // A wakeup makes SDL_WaitEvent return so the loop re-checks HasQuitFromUIThread.
  NotifyUILoopOfPendingFunctions();
}

int SDL3WindowedAppContext::RunMainMessageLoop() {
  while (!HasQuitFromUIThread()) {
    SDL_Event event;
    if (!SDL_WaitEvent(&event)) {
      REX_FAIL(ui, EXIT_FAILURE, "SDL_WaitEvent failed: {}", SDL_GetError());
    }
    ProcessEvent(event);
  }
  return EXIT_SUCCESS;
}

void SDL3WindowedAppContext::PumpEvents(std::chrono::milliseconds max_wait) {
  SDL_Event event;
  if (!SDL_WaitEventTimeout(&event, static_cast<i32>(max_wait.count()))) {
    return;
  }
  ProcessEvent(event);
  while (!HasQuitFromUIThread() && SDL_PollEvent(&event)) {
    ProcessEvent(event);
  }
}

void SDL3WindowedAppContext::ProcessEvent(SDL_Event& event) {
  if (event.type == wakeup_event_type_) {
    ExecutePendingFunctionsFromUIThread();
    return;
  }
  if (event.type == paint_event_type_) {
    // A queued quit goes first, pumped so a stream of paints cannot starve Cocoa.
    SDL_PumpEvents();
    SDL_Event quit_event{};
    if (SDL_PeepEvents(&quit_event, 1, SDL_GETEVENT, SDL_EVENT_QUIT, SDL_EVENT_QUIT) > 0) {
      ProcessEvent(quit_event);
      if (HasQuitFromUIThread()) {
        return;
      }
    }
    if (WindowSDL* window = GetWindow(event.user.windowID)) {
      window->HandlePaintEvent();
    }
    return;
  }
  if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) {
    if (WindowSDL* window = GetWindow(event.window.windowID)) {
      window->HandleWindowEvent(event);
    }
    return;
  }
  switch (event.type) {
    case SDL_EVENT_QUIT:
      if (synchronously_handled_quit_events_ != 0) {
        --synchronously_handled_quit_events_;
      } else {
        ProcessQuitRequest();
      }
      break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      if (WindowSDL* window = GetWindow(event.key.windowID)) {
        window->HandleKeyEvent(event);
      }
      break;
    }
    case SDL_EVENT_TEXT_INPUT: {
      if (WindowSDL* window = GetWindow(event.text.windowID)) {
        window->HandleTextInputEvent(event);
      }
      break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
      if (WindowSDL* window = GetWindow(event.motion.windowID)) {
        window->HandleMouseEvent(event);
      }
      break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      if (WindowSDL* window = GetWindow(event.button.windowID)) {
        window->HandleMouseEvent(event);
      }
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      if (WindowSDL* window = GetWindow(event.wheel.windowID)) {
        window->HandleMouseEvent(event);
      }
      break;
    }
    case SDL_EVENT_DROP_FILE: {
      if (WindowSDL* window = GetWindow(event.drop.windowID)) {
        window->HandleDropEvent(event);
      }
      break;
    }
    default:
      break;
  }
}

bool SDLCALL SDL3WindowedAppContext::WatchEvent(void* userdata, SDL_Event* event) {
  auto* context = static_cast<SDL3WindowedAppContext*>(userdata);
  if (event->type == SDL_EVENT_QUIT && SDL_IsMainThread() && context->IsInUIThread()) {
    // Handled while SDL queues the quit, before rendering blocks in nextDrawable.
    ++context->synchronously_handled_quit_events_;
    context->ProcessQuitRequest();
  }
  return true;
}

void SDL3WindowedAppContext::ProcessQuitRequest() {
  // Goes through the close request path so OnClosing stops the guest, audio and GPU threads.
  std::vector<SDL_WindowID> window_ids;
  window_ids.reserve(windows_.size());
  for (const auto& [id, window] : windows_) {
    (void)window;
    window_ids.push_back(id);
  }
  for (SDL_WindowID id : window_ids) {
    if (WindowSDL* window = GetWindow(id)) {
      SDL_Event close_event{};
      close_event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
      close_event.window.windowID = id;
      window->HandleWindowEvent(close_event);
    }
  }
}

}  // namespace rex::ui
