/**
 * @file        gpu/ui_thread.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/move_only_function.h>

namespace rex::gpu {

/** The thread that owns windows and presentation. */
class IUIThread {
 public:
  virtual ~IUIThread() = default;

  /**
   * Runs work on the UI thread and returns after it finishes.
   *
   * @param work The work to run.
   */
  virtual void RunSynchronous(rex::MoveOnlyFunction<void()> work) = 0;
};

}  // namespace rex::gpu
