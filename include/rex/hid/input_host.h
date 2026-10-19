/**
 * @file        hid/input_host.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <optional>

#include <rex/hid/event.h>
#include <rex/move_only_function.h>
#include <rex/module.h>

namespace rex::hid {

/** The window a HID backend serves. Implemented by the app that owns the window. */
REX_INTERFACE(IInputHost)
 public:
  /**
   * Queues work to run on the UI thread later. Safe on any thread.
   *
   * @param work The function to run on the UI thread.
   * @return True when the work is queued, false when the UI thread no longer takes work.
   */
  virtual bool Post(rex::MoveOnlyFunction<void()> work) = 0;

  /**
   * Applies a pointer mode. UI thread only.
   *
   * @param mode The pointer mode to apply.
   * @return True when Locked delivers relative motion, false when it falls back to recentering.
   */
  virtual bool ApplyPointerMode(PointerMode mode) = 0;

  /**
   * Warps the pointer to the window center. UI thread only.
   *
   * @return The pointer state after the warp, or empty when the warp failed.
   */
  virtual std::optional<PointerState> WarpPointerToCenter() = 0;

  /**
   * Reports whether pad and keyboard input reach the title. Safe on any thread.
   *
   * @return True when pad and keyboard input reach the title.
   */
  virtual bool GetCaptureActive() const = 0;
};

}  // namespace rex::hid
