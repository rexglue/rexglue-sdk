/**
 * @file        ui/ui_control.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>

#include <rex/module.h>
#include <rex/result.h>
#include <rex/title.h>
#include <rex/types.h>

namespace rex {
class IAchievementRegistry;
}  // namespace rex

namespace rex::ui {

/** The UI calls of the title runtime and the app. Not part of the public API. */
REX_DECLARE_PRIVATE_INTERFACE(IUIControl, 1)
/**
 * @param achievements The achievements the overlays show.
 * @return Ok, or the error that stops the launch.
 */
  [[nodiscard]] virtual Result<void> AttachTitle(const TitleInfo& info,
                                                 IAchievementRegistry& achievements) = 0;

  virtual void DetachTitle() = 0;

  /** Ends the UI frame loop and wakes it. */
  virtual void EndFrameLoop() = 0;

  /**
   * Sets the function the UI calls when the player closes the window or the driver ends. The app
   * turns it into a shutdown request.
   *
   * @param handler The function, or an empty one for none. Set and cleared on the UI thread.
   */
  virtual void SetCloseHandler(MoveOnlyFunction<void()> handler) = 0;

  /**
   * Sets the function the UI calls when the window changed size. The app points it at the graphics
   * system.
   *
   * @param handler The function, or an empty one for none. It receives the client width and
   *        height.
   */
  virtual void SetResizeHandler(MoveOnlyFunction<void(u32, u32)> handler) = 0;

  /**
   * Runs the posted work, then waits for more. UI thread only.
   *
   * @param timeout The longest wait for new work.
   * @return False when this is not the UI thread or the system is not started.
   */
  virtual bool ServePosts(std::chrono::milliseconds timeout) = 0;

  /** Begins shutdown on the UI thread. The prompts close and DrawOverlays builds no more frames. */
  virtual void BeginStop() = 0;

  /**
   * Runs one frame of the loop.
   *
   * @param timeout_ms The longest wait for events.
   * @return False when the loop ended.
   */
  virtual bool RunFrame(u32 timeout_ms) = 0;
};

}  // namespace rex::ui
