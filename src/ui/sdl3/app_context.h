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

#include <utility>

#include <vector>

#include <rex/api.h>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

#include <rex/assert.h>
#include <rex/macros.h>
#include <rex/platform/thread.h>
#include <rex/types.h>
#include <rex/move_only_function.h>

namespace rex {
namespace ui {

/** Context for the entry point inputs and the platform UI loop, implemented per platform. */
class WindowedAppContext : public rex::NonCopyable {
 public:
  virtual ~WindowedAppContext();

  /**
   * Reports whether the calling thread is the UI thread, which created the object.
   *
   * @return True when called from the UI thread.
   */
  bool IsInUIThread() const { return std::this_thread::get_id() == ui_thread_id_; }

  /**
   * Enqueues the function regardless of the current thread, to run when the UI thread runs the loop
   * or ExecutePendingFunctionsFromUIThread. Captured references can outlive their owners, so wait
   * on a fence, or call ExecutePendingFunctionsFromUIThread in the UI thread, before destroying
   * them. May be called from queued functions. The platform loop is notified even while pending
   * functions run, since they may run inner modal loops. Calls nothing virtual during destruction.
   *
   * @param function Function to run on the UI thread.
   * @return True when enqueued, false when dropped because the loop has exited.
   */
  bool CallInUIThreadDeferred(rex::MoveOnlyFunction<void()> function);
  /**
   * Runs the function immediately in the UI thread and enqueues it otherwise.
   *
   * @param function Function to run on the UI thread.
   * @return True when run or enqueued, false when dropped because the loop has exited.
   */
  bool CallInUIThread(rex::MoveOnlyFunction<void()> function);
  /**
   * Runs the function in the UI thread and waits for it. Safe to call from queued functions.
   *
   * @tparam F Callable type.
   * @param function Function to run on the UI thread.
   * @return True when the function ran, false when dropped because the loop has exited.
   */
  template <std::invocable<> F>
  bool CallInUIThreadSynchronous(F&& function) {
    if (IsInUIThread()) {
      // Prevent deadlock if called from the UI thread.
      function();
      return true;
    }
    rex::platform::Fence fence;
    if (!CallInUIThreadDeferred([&function, &fence]() {
          function();
          fence.Signal();
        })) {
      return false;
    }
    fence.Wait();
    return true;
  }
  /** Runs the queued functions now. Safe to call from queued functions. */
  void ExecutePendingFunctionsFromUIThread() { ExecutePendingFunctionsFromUIThread(false); }

  /**
   * Reports whether a quit was requested, including during the destructor, when queued functions
   * may be running.
   *
   * @return True once QuitFromUIThread ran or the destructor started.
   */
  bool HasQuitFromUIThread() const {
    REX_ASSERT(IsInUIThread());
    return has_quit_;
  }

  /**
   * Disallows new pending functions, runs the queued ones and asks the platform loop to stop. Does
   * not destroy the context. May call virtual functions, so the destructor runs
   * ExecutePendingFunctionsFromUIThread(true) instead. Safe to call from pending functions.
   */
  void QuitFromUIThread();

 protected:
  WindowedAppContext() : ui_thread_id_(std::this_thread::get_id()) {}

  /**
   * Asks the OS to run a UI loop iteration so pending functions run. Callable from any thread and
   * may run after PlatformQuitFromUIThread. pending_functions_mutex_ is not held, since a blocked
   * write to a full message pipe would deadlock with the UI thread that drains it.
   */
  virtual void NotifyUILoopOfPendingFunctions() = 0;

  /**
   * Tells the platform to terminate the UI loop. The queue is empty and accepts no new functions,
   * but NotifyUILoopOfPendingFunctions may be called.
   */
  virtual void PlatformQuitFromUIThread() = 0;

  std::thread::id ui_thread_id_;

 private:
  /**
   * Runs the queued functions. Calls nothing virtual when is_final is set from the destructor.
   *
   * @param is_final True to stop accepting new functions.
   */
  void ExecutePendingFunctionsFromUIThread(bool is_final);

  /** Accessed by the UI thread only. */
  bool has_quit_ = false;
  bool is_in_destructor_ = false;

  /** Guards pending_functions_ and pending_functions_accepted_. */
  std::mutex pending_functions_mutex_;
  std::deque<rex::MoveOnlyFunction<void()>> pending_functions_;
  /**
   * Guarded by pending_functions_mutex_, written by the UI thread. Set to false before the platform
   * loop exits and after all pending functions ran, since they may signal awaited fences.
   */
  bool pending_functions_accepted_ = true;
};

}  // namespace ui
}  // namespace rex
