/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <thread>

#include <rex/platform/thread.h>

namespace rex::platform {

u32 LogicalProcessorCount() {
  static u32 value = 0;
  if (!value) {
    value = std::thread::hardware_concurrency();
  }
  return value;
}

thread_local u32 current_thread_id = UINT_MAX;

u32 CurrentThreadId() {
  return current_thread_id == UINT_MAX ? CurrentThreadSystemId() : current_thread_id;
}

void SetCurrentThreadId(u32 id) {
  current_thread_id = id;
}

}  // namespace rex::platform
