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

#include <rex/system/thread.h>
#include <rex/system/thread_state.h>

namespace rex::runtime {

thread_local Thread* Thread::current_thread_ = nullptr;

Thread::Thread() {}
Thread::~Thread() {}

bool Thread::IsInThread() {
  return current_thread_ != nullptr;
}

Thread* Thread::GetCurrentThread() {
  return current_thread_;
}
uint32_t Thread::GetCurrentThreadId() {
  return Thread::GetCurrentThread()->thread_state()->thread_id();
}

}  // namespace rex::runtime