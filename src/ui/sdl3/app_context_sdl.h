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

#include <rex/platform.h>
#include <rex/types.h>
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

  /**
   * Initializes the SDL video subsystem and registers the custom event types.
   *
   * @param config UI configuration.
   * @return True on success.
   */
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

  /**
   * Returns the custom SDL event type for a coalesced paint request. user.windowID identifies the
   * target window.
   *
   * @return The event type.
   */
  u32 GetPaintEventType() const { return paint_event_type_; }

  /**
   * Registers a window for event routing. UI thread only.
   *
   * @param id SDL window id.
   * @param window Window that receives the events.
   */
  void RegisterWindow(SDL_WindowID id, WindowSDL* window) { windows_.emplace(id, window); }
  void UnregisterWindow(SDL_WindowID id) { windows_.erase(id); }

 private:
  WindowSDL* GetWindow(SDL_WindowID id) const {
    auto it = windows_.find(id);
    return it != windows_.end() ? it->second : nullptr;
  }

  /**
   * Dispatches an SDL event. A paint event first drains a queued quit request, since CAMetalLayer
   * nextDrawable can block forever once Cocoa requests termination. The pump is needed because a
   * continuous stream of paint events keeps SDL_WaitEvent from returning to Cocoa to collect the
   * application menu quit.
   *
   * @param event Event to dispatch.
   */
  void ProcessEvent(SDL_Event& event);
  /**
   * Requests close on every window through the normal close path, since OnClosing stops the title,
   * audio and GPU threads and bypassing it hangs teardown.
   */
  void ProcessQuitRequest();
  /**
   * SDL event watch. On the main thread a quit event is handled synchronously while SDL queues it,
   * before rendering can block in nextDrawable.
   *
   * @param userdata The SDLWindowedAppContext.
   * @param event Event being queued.
   * @return Always true.
   */
  static bool SDLCALL WatchEvent(void* userdata, SDL_Event* event);

  std::unordered_map<SDL_WindowID, WindowSDL*> windows_;
  u32 wakeup_event_type_ = 0;
  u32 paint_event_type_ = 0;
  u32 synchronously_handled_quit_events_ = 0;
  bool event_watch_registered_ = false;
};

}  // namespace rex::ui
