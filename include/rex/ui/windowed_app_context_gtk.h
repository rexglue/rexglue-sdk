/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <mutex>

#include <rex/ui/windowed_app_context.h>

#include <glib.h>

namespace rex {
namespace ui {

class GTKWindowedAppContext final : public WindowedAppContext {
 public:
  GTKWindowedAppContext() = default;
  ~GTKWindowedAppContext();

  void NotifyUILoopOfPendingFunctions() override;

  void PlatformQuitFromUIThread() override;

  void RunMainGTKLoop();

 private:
  static gboolean PendingFunctionsSourceFunc(gpointer data);

  static gboolean QuitSourceFunc(gpointer data);

  std::mutex pending_functions_idle_pending_mutex_;
  guint pending_functions_idle_pending_ = 0;

  guint quit_idle_pending_ = 0;
};

}  // namespace ui
}  // namespace rex
