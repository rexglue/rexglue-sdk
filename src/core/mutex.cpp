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

namespace {

// The OS thread id of the thread holding the global lock, 0 if none or not
// known. glibc keeps the owner's id in the mutex itself (it needs it for
// recursion), so knowing it costs nothing on the uncontended path; it may
// change before a waiter blocks, which a statistic can live with.
uint32_t Holder() {
#if defined(__GLIBC__)
  return uint32_t(__atomic_load_n(&global_critical_region::mutex().native_handle()->__data.__owner,
                                  __ATOMIC_RELAXED));
#else
  return 0;
#endif
}

}  // namespace

void global_critical_region::LockContended(std::unique_lock<std::recursive_mutex>& lock) {
  uint32_t holder = Holder();
  if (holder == 0) {
    // Released since the first try: take it now if nobody else did, without
    // counting a wait; else see who did.
    if (lock.try_lock()) return;
    holder = Holder();
  }
  const uint64_t start = thread_stats::NowMicros();
  lock.lock();
  const uint64_t waited = thread_stats::NowMicros() - start;
  thread_stats::AddWait(thread_stats::Wait::kGlobalLock, waited);
  thread_stats::AddLockHolderWait(holder, waited);
}

}  // namespace rex::thread
