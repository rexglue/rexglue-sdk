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

u32 logical_processor_count() {
  static u32 value = 0;
  if (!value) {
    value = std::thread::hardware_concurrency();
  }
  return value;
}

thread_local u32 current_thread_id_ = UINT_MAX;

u32 current_thread_id() {
  return current_thread_id_ == UINT_MAX ? current_thread_system_id() : current_thread_id_;
}

void set_current_thread_id(u32 id) {
  current_thread_id_ = id;
}

}  // namespace rex::platform
