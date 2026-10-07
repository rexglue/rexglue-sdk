/**
 * @file        ui/sdl3/app_context_sdl.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/macros.h>
#include <rex/ui.h>

#include <chrono>
#include <cstdint>
#include <unordered_map>

#include <SDL3/SDL_events.h>

#include "ui/sdl3/app_context.h"

namespace rex::ui {

class WindowSDL;

class SDLWindowedAppContext final : public WindowedAppContext {
 public:
  SDLWindowedAppContext() = default;
  ~SDLWindowedAppContext() override;

  // Initializes the SDL video subsystem and registers the custom event types.
  // Must be called (and must succeed) before any other use.
  bool Initialize(const UIConfig& config);

  void NotifyUILoopOfPendingFunctions() override;
  void PlatformQuitFromUIThread() override;

  int RunMainMessageLoop();

  /**
   * Processes the events that arrive within a wait, then every event already queued.
   *
   * @param max_wait Longest time to wait for the first event.
   */
  void PumpEvents(std::chrono::milliseconds max_wait);

  // Custom SDL event type carrying a coalesced paint request for a window
  // (user.windowID identifies the target).
  uint32_t paint_event_type() const { return paint_event_type_; }

  // Window registry for event routing. UI thread only.
  void RegisterWindow(SDL_WindowID id, WindowSDL* window) { windows_.emplace(id, window); }
  void UnregisterWindow(SDL_WindowID id) { windows_.erase(id); }

 private:
  WindowSDL* GetWindow(SDL_WindowID id) const {
    auto it = windows_.find(id);
    return it != windows_.end() ? it->second : nullptr;
  }

  void ProcessEvent(SDL_Event& event);
  void ProcessQuitRequest();
  static bool SDLCALL WatchEvent(void* userdata, SDL_Event* event);

  std::unordered_map<SDL_WindowID, WindowSDL*> windows_;
  uint32_t wakeup_event_type_ = 0;
  uint32_t paint_event_type_ = 0;
  uint32_t synchronously_handled_quit_events_ = 0;
  bool event_watch_registered_ = false;
};

}  // namespace rex::ui
