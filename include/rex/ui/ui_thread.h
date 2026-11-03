/**
 * @file        ui/ui_thread.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/module.h>
#include <rex/utility.h>

namespace rex {

/** The UI thread operations other modules call. IUISystem declares the same calls. */
REX_DECLARE_INTERFACE(IUIThread, 1)
/** Returns whether the caller is on the UI thread. Any thread. */
  virtual bool IsUIThread() const = 0;

  /** Queues a function to run on the UI thread between frames. Any thread. */
  virtual void PostTask(MoveOnlyFunction<void()> fn) = 0;

  /**
   * Runs a function on the UI thread and waits for it. Runs it at once when called on the UI
   * thread. Any thread.
   *
   * @return False when the UI system stopped and the function did not run.
   */
  [[nodiscard]] virtual bool RunSynchronous(MoveOnlyFunction<void()> fn) = 0;
};

}  // namespace rex
