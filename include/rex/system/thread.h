#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2017 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdint>

#include <rex/thread.h>

namespace rex::runtime {

class ThreadState;

// Represents a thread that runs guest code.
class Thread {
 public:
  Thread();
  ~Thread();

  static bool IsInThread();
  static Thread* GetCurrentThread();
  static uint32_t GetCurrentThreadId();
  ThreadState* thread_state() const { return thread_state_; }

  // True if the thread should be paused by the debugger.
  // All threads that can run guest code must be stopped for the debugger to
  // work properly.
  bool can_debugger_suspend() const { return can_debugger_suspend_; }
  void set_can_debugger_suspend(bool value) { can_debugger_suspend_ = value; }

  rex::thread::Thread* thread() { return thread_.get(); }
  const std::string& thread_name() const { return thread_name_; }

 protected:
  thread_local static Thread* current_thread_;

  ThreadState* thread_state_ = nullptr;
  std::unique_ptr<rex::thread::Thread> thread_ = nullptr;

  bool can_debugger_suspend_ = true;
  std::string thread_name_;
};

}  // namespace rex::runtime
