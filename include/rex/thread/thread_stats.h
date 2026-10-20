/**
 * @file        rex/thread/thread_stats.h
 *
 * @brief       How long each host thread spends waiting, and on what, for an
 *              in-engine profiler in Release builds.
 *
 * A thread's CPU time (from the OS) says how busy it is; it doesn't say why a
 * thread that should be busy isn't. These totals add that: the time each
 * thread spent blocked in a guest wait (an event, semaphore or thread the
 * game waits on), a guest sleep, a contended guest spinlock (spinning counts
 * as CPU time, so it is the one wait that looks like work) and the SDK's
 * global lock.
 *
 * Totals only grow. A reader takes two snapshots some time apart and
 * subtracts. Threads are keyed by their OS id (Linux tid, Windows thread id),
 * so a reader can match them with the OS's own per-thread CPU times and
 * names. A thread gets its slot the first time it waits; slots are never
 * reused (a game makes a few dozen threads), and once all are taken further
 * threads simply aren't counted.
 *
 * Cost: two clock reads per blocking wait; nothing on an uncontended lock.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <cstdint>

// A game built against SDKs with and without these totals can test this.
// 2: who held the global lock while a thread waited for it (ReadLockHolders).
#define REX_THREAD_STATS_VERSION 2

namespace rex::thread_stats {

enum class Wait : uint32_t {
  kGuestObject,  // NtWaitFor*/KeWaitFor*: the game waits on one of its own objects
  kGuestSleep,   // KeDelayExecutionThread with a delay
  kSpinLock,     // spinning on a guest spinlock another thread holds
  kGlobalLock,   // blocked on the SDK's global critical region
  kCount
};

struct ThreadTotals {
  uint32_t system_id = 0;  // OS thread id
  uint64_t wait_us[size_t(Wait::kCount)] = {};
  uint64_t waits[size_t(Wait::kCount)] = {};
};

// The most threads counted.
inline constexpr size_t kMaxThreads = 512;

// Adds a finished wait of the calling thread.
void AddWait(Wait kind, uint64_t micros);

// Copies the totals of every thread counted so far into `out` (up to
// `max_threads`); returns how many were written.
size_t Read(ThreadTotals* out, size_t max_threads);

// "guest wait", "guest sleep", "spinlock", "global lock".
const char* Name(Wait kind);

// The global lock waits of one thread, by the thread that held the lock when
// the wait began: a thread that waits a lot says who to look at. The holder
// is read from the lock itself, which only Linux (glibc) shows; elsewhere,
// and past kMaxHoldersPerThread holders, it is 0 (not known).
struct LockHolderTotals {
  uint32_t waiter_id = 0;  // OS thread id of the thread that waited
  uint32_t holder_id = 0;  // OS thread id of the thread holding the lock, 0 = not known
  uint64_t wait_us = 0;
  uint64_t waits = 0;
};

inline constexpr size_t kMaxHoldersPerThread = 12;

// Adds a finished global lock wait of the calling thread (the time is also
// added with AddWait(Wait::kGlobalLock, ...) by the caller).
void AddLockHolderWait(uint32_t holder_id, uint64_t micros);

// Copies every (waiter, holder) total so far into `out`, up to `max_pairs`;
// returns how many were written.
size_t ReadLockHolders(LockHolderTotals* out, size_t max_pairs);

// Steady clock in microseconds (the clock these totals use).
uint64_t NowMicros();

// Adds its own lifetime to the calling thread's totals.
class ScopedWait {
 public:
  explicit ScopedWait(Wait kind) : kind_(kind), start_us_(NowMicros()) {}
  ~ScopedWait() { AddWait(kind_, NowMicros() - start_us_); }
  ScopedWait(const ScopedWait&) = delete;
  ScopedWait& operator=(const ScopedWait&) = delete;

 private:
  Wait kind_;
  uint64_t start_us_;
};

}  // namespace rex::thread_stats
