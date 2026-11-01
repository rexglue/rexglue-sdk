/**
 * @file        base/driver_context.h
 * @brief       The context an app hands every driver it starts
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <thread>

#include <rex/driver_context.h>
#include <rex/ui/ui_thread.h>
#include <rex/module.h>
#include <rex/platform.h>
#include <rex/utility.h>

namespace rex {

/**
 * Without a UI, GetSurface fails and RunOnUIThread and PostToUIThread run the function only on
 * the thread that created the context. From another thread RunOnUIThread fails and
 * PostToUIThread discards the function.
 */
class REX_API AppDriverContext final : public IDriverContext {
 public:
  AppDriverContext(ISettings& settings, ILogger& logger);

  ISettings& GetSettings() override;
  ILogger& GetLogger() override;
  [[nodiscard]] Result<void> RunOnUIThread(MoveOnlyFunction<void()> fn) override;
  void PostToUIThread(MoveOnlyFunction<void()> fn) override;

  /**
   * Sets the UI that serves RunOnUIThread and PostToUIThread.
   *
   * @param ui The UI system, or null to serve neither.
   */
  void SetUI(IUIThread* ui);

 private:
  ISettings& settings_;
  ILogger& logger_;
  IUIThread* ui_ = nullptr;
  std::thread::id init_thread_;
};

}  // namespace rex
