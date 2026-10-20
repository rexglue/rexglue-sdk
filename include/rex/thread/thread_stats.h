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
#define REX_THREAD_STATS_VERSION 1

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
