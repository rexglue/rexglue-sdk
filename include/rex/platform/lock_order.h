/**
 * @file        platform/lock_order.h
 * @brief       Ranked mutexes with a debug lock order check
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <mutex>
#include <shared_mutex>

#include <rex/macros.h>
#include <rex/types.h>

namespace rex::platform {

/**
 * Ranks of the kernel locks in the order they may be taken. A lock may be taken only when its rank
 * is greater than every rank the thread already holds. Leaf may be taken after any rank and nothing
 * may be taken while a Leaf is held.
 */
enum class LockRank : u8 {
  Identity = 1,
  Table,
  Name,
  ThreadRegistry,
  ModuleRegistry,
  Object,
  Leaf,
};

/**
 * Receives an order violation in a debug build.
 *
 * @param held The rank on top of the thread's held stack.
 * @param acquiring The rank the thread is taking.
 */
using LockOrderHandler = void (*)(LockRank held, LockRank acquiring);

/**
 * Replaces the violation handler. The default handler asserts.
 *
 * @param handler The new handler, or null to restore the default.
 * @return The previous handler.
 */
REX_API LockOrderHandler SetLockOrderHandler(LockOrderHandler handler);

/**
 * Records an acquire on the calling thread and reports an order violation in a debug build.
 *
 * @param rank The rank being taken.
 */
REX_API void NoteLockAcquire(LockRank rank);

/**
 * Records a release on the calling thread.
 *
 * @param rank The rank being released.
 */
REX_API void NoteLockRelease(LockRank rank);

/** A non recursive mutex that reports order violations in a debug build. */
class RankedMutex {
 public:
  /**
   * @param rank The rank of this lock.
   */
  explicit RankedMutex(LockRank rank) : rank_(rank) {}
  RankedMutex(const RankedMutex&) = delete;
  RankedMutex& operator=(const RankedMutex&) = delete;

  /** Locks the mutex. */
  void lock() {
    NoteLockAcquire(rank_);
    mutex_.lock();
  }

  /**
   * Tries to lock the mutex.
   *
   * @return True when the mutex was locked.
   */
  bool try_lock() {
    if (!mutex_.try_lock()) {
      return false;
    }
    NoteLockAcquire(rank_);
    return true;
  }

  /** Unlocks the mutex. */
  void unlock() {
    NoteLockRelease(rank_);
    mutex_.unlock();
  }

 private:
  std::mutex mutex_;
  LockRank rank_;
};

/** A non recursive shared mutex that reports order violations in a debug build. */
class RankedSharedMutex {
 public:
  /**
   * @param rank The rank of this lock.
   */
  explicit RankedSharedMutex(LockRank rank) : rank_(rank) {}
  RankedSharedMutex(const RankedSharedMutex&) = delete;
  RankedSharedMutex& operator=(const RankedSharedMutex&) = delete;

  /** Locks the mutex exclusively. */
  void lock() {
    NoteLockAcquire(rank_);
    mutex_.lock();
  }

  /** Unlocks the exclusive lock. */
  void unlock() {
    NoteLockRelease(rank_);
    mutex_.unlock();
  }

  /** Locks the mutex for shared use. */
  void lock_shared() {
    NoteLockAcquire(rank_);
    mutex_.lock_shared();
  }

  /** Unlocks the shared lock. */
  void unlock_shared() {
    NoteLockRelease(rank_);
    mutex_.unlock_shared();
  }

 private:
  std::shared_mutex mutex_;
  LockRank rank_;
};

}  // namespace rex::platform
