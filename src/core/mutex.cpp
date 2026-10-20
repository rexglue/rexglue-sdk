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

#include <rex/cvar.h>
#include <rex/thread.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

REXCVAR_DEFINE_INT32(global_lock_spin_us, -1, "Kernel",
                     "How long a thread that finds the SDK's global lock taken keeps checking "
                     "for it before it sleeps, in microseconds. The lock is held for short "
                     "stretches (a kernel call's bookkeeping), and a sleeping thread takes longer "
                     "to wake than most holds last. -1 = auto: 30 with 6 or more logical CPUs, 0 "
                     "(sleep at once) with fewer")
    .range(-1, 1000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

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

// Whether the lock looks free (glibc: its futex word is 0), so a waiter only
// writes to the lock's cache line with a try when it may succeed. Elsewhere,
// always try.
bool LooksFree() {
#if defined(__GLIBC__)
  return __atomic_load_n(&global_critical_region::mutex().native_handle()->__data.__lock,
                         __ATOMIC_RELAXED) == 0;
#else
  return true;
#endif
}

void CpuRelax() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  _mm_pause();
#elif defined(__aarch64__)
  __asm__ __volatile__("yield");
#endif
}

// How long to spin before sleeping (global_lock_spin_us). Spinning only pays
// with cores to spare: on a 2-4 core PC the holder may be waiting for the very
// core the spinner takes.
uint64_t SpinMicros() {
  const int32_t setting = REXCVAR_GET(global_lock_spin_us);
  if (setting >= 0) return uint64_t(setting);
  static const uint64_t by_machine = logical_processor_count() >= 6 ? 30 : 0;
  return by_machine;
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
  // Spin first: the kernel calls that take the region hold it for a few
  // microseconds, while blocking costs a sleep and a wake-up (tens of
  // microseconds, more from a deep idle state) on top of the hold. Without
  // this, a thread that takes the region thousands of times a frame (the GPU
  // emulation, for every draw's memory ranges) lost 10-20% of its time
  // waiting on the owner's PC (2026-10-15 reports).
  bool got = false;
  if (const uint64_t spin_us = SpinMicros(); spin_us > 0) {
    for (uint32_t i = 1;; ++i) {
      CpuRelax();
      if (LooksFree() && lock.try_lock()) {
        got = true;
        break;
      }
      // The clock is read every 64 checks (a few microseconds).
      if ((i & 63) == 0 && thread_stats::NowMicros() - start >= spin_us) break;
    }
  }
  if (!got) lock.lock();
  const uint64_t waited = thread_stats::NowMicros() - start;
  thread_stats::AddWait(thread_stats::Wait::kGlobalLock, waited);
  thread_stats::AddLockHolderWait(holder, waited);
  thread_stats::AddLockSpin(got);
}

}  // namespace rex::thread
