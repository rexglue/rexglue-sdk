/**
 * @file        driver_context.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/module.h>
#include <rex/result.h>
#include <rex/utility.h>

namespace rex {

class ILogger;
class ISettingsRegistry;

/** What a driver receives at Initialize, and its only route to core services. */
REX_DECLARE_INTERFACE(IDriverContext, 1)
/** Returns the settings registry, where the driver registers its own settings. */
  virtual ISettingsRegistry& GetSettingsRegistry() = 0;

  /**
   * Returns the logger, where the driver adds its categories.
   */
  virtual ILogger& GetLogger() = 0;

  /**
   * Runs a function on the UI thread and waits for it. Runs it at once when called on the UI
   * thread. Never call it from a thread the UI thread may wait for.
   *
   * @return Ok when it ran, or an error when the UI system stopped or there is no UI thread.
   */
  [[nodiscard]] virtual Result<void> RunOnUIThread(MoveOnlyFunction<void()> fn) = 0;

  /**
   * Queues a function for the UI thread and returns at once. Any thread. A function queued after
   * the UI thread stopped may never run.
   */
  virtual void PostToUIThread(MoveOnlyFunction<void()> fn) = 0;
};

}  // namespace rex
