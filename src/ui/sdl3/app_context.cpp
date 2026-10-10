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

#include <utility>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/no_throw.h>
#include <rex/platform/thread.h>
#include <rex/move_only_function.h>
#include "ui/sdl3/app_context.h"

namespace rex {
namespace ui {

template <typename Run>
void WindowedAppContext::DrainPendingFunctions(bool is_final, Run run) {
  REX_ASSERT(IsInUIThread());
  pending_functions_mutex_.lock();
  while (!pending_functions_.empty()) {
    // Popped before running, a nested ExecutePendingFunctionsFromUIThread would rerun it.
    rex::MoveOnlyFunction<void()> function = std::move(pending_functions_.front());
    pending_functions_.pop_front();
    // Run unlocked, a function with a modal loop would otherwise block new functions.
    pending_functions_mutex_.unlock();
    run(function);
    pending_functions_mutex_.lock();
  }
  if (is_final) {
    // Closed atomically with the drain, a function queued later would never run.
    pending_functions_accepted_ = false;
  }
  pending_functions_mutex_.unlock();
}

WindowedAppContext::~WindowedAppContext() {
  REX_ASSERT(IsInUIThread());

  // Must not run from a queued function, which would touch the destroyed queue and mutex.

  // Stops CallInUIThreadDeferred from calling the virtual NotifyUILoopOfPendingFunctions.
  is_in_destructor_ = true;
  // Stops the final execution from calling the virtual PlatformQuitFromUIThread.
  has_quit_ = true;

  // The subclass performs the platform quit, PlatformQuitFromUIThread is no longer callable.
  DrainPendingFunctions(true, [](rex::MoveOnlyFunction<void()>& function) noexcept {
    log::NoThrow(log::ui, function);
  });
}

bool WindowedAppContext::CallInUIThreadDeferred(rex::MoveOnlyFunction<void()> function) {
  {
    std::unique_lock<std::mutex> pending_functions_lock(pending_functions_mutex_);
    if (!pending_functions_accepted_) {
      return false;
    }
    pending_functions_.emplace_back(std::move(function));
  }
  // Notified unlocked, a blocking notify would deadlock with the UI thread waiting on the mutex.
  if (!is_in_destructor_) {
    NotifyUILoopOfPendingFunctions();
  }
  return true;
}

bool WindowedAppContext::CallInUIThread(rex::MoveOnlyFunction<void()> function) {
  if (IsInUIThread()) {
    function();
    return true;
  }
  return CallInUIThreadDeferred(std::move(function));
}

void WindowedAppContext::QuitFromUIThread() {
  REX_ASSERT(IsInUIThread());
  bool has_quit_previously = has_quit_;
  // PlatformQuitFromUIThread runs once, not from nested executions during the quit.
  has_quit_ = true;
  ExecutePendingFunctionsFromUIThread(true);
  if (has_quit_previously) {
    // Re-entered from a pending function, the outer call performs the platform quit.
    return;
  }
  // New functions are rejected from here, NotifyUILoopOfPendingFunctions may be called.
  PlatformQuitFromUIThread();
}

void WindowedAppContext::ExecutePendingFunctionsFromUIThread(bool is_final) {
  DrainPendingFunctions(is_final, [](rex::MoveOnlyFunction<void()>& function) { function(); });
}

}  // namespace ui
}  // namespace rex
