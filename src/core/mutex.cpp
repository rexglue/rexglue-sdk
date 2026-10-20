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

#include <rex/thread/mutex.h>
#include <rex/thread/thread_stats.h>

namespace rex::thread {

std::recursive_mutex& global_critical_region::mutex() {
  static std::recursive_mutex global_mutex;
  return global_mutex;
}

void global_critical_region::LockContended(std::unique_lock<std::recursive_mutex>& lock) {
  thread_stats::ScopedWait waited(thread_stats::Wait::kGlobalLock);
  lock.lock();
}

}  // namespace rex::thread
