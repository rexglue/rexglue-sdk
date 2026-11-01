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

u32 GetLogicalProcessorCount() {
  static u32 value = 0;
  if (!value) {
    value = std::thread::hardware_concurrency();
  }
  return value;
}

thread_local u32 g_current_thread_id = UINT_MAX;

u32 GetCallerThreadID() {
  return g_current_thread_id == UINT_MAX ? GetCallerSystemThreadID() : g_current_thread_id;
}

void SetCallerThreadID(u32 id) {
  g_current_thread_id = id;
}

}  // namespace rex::platform
