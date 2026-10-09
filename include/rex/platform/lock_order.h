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
  Interrupt = 1,  /**< Rank 1, the lowest. Guards the function dispatcher interrupt state. */
  Identity,       /**< Rank 2. Guards the object table identity map and the module load counts. */
  Table,          /**< Rank 3. Guards the object table. */
  Name,           /**< Rank 4. Guards the object table name map. */
  ThreadRegistry, /**< Rank 5. Guards the thread registry. */
  ModuleRegistry, /**< Rank 6. Guards the module registry. */
  Object,         /**< Rank 7. Guards the state of a single object, such as the APU. */
  DPC,            /**< Rank 8. Guards the deferred procedure call queue of the dispatcher. */
  Leaf,           /**< Rank 9, the highest. Taken last, nothing may be taken while it is held. */
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
 * Pushes the rank of a lock the calling thread takes and reports an order violation in a debug
 * build.
 *
 * @param rank The rank being taken.
 */
REX_API void PushLockRank(LockRank rank);

/**
 * Pops the rank of a lock the calling thread releases.
 *
 * @param rank The rank being released.
 */
REX_API void PopLockRank(LockRank rank);

/** A non recursive mutex that reports order violations in a debug build. */
class RankedMutex : public rex::NonCopyable {
 public:
  /**
   * @param rank The rank of this lock.
   */
  explicit RankedMutex(LockRank rank) : rank_(rank) {}

  /** Locks the mutex. */
  void lock() {
    PushLockRank(rank_);
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
    PushLockRank(rank_);
    return true;
  }

  /** Unlocks the mutex. */
  void unlock() {
    PopLockRank(rank_);
    mutex_.unlock();
  }

 private:
  std::mutex mutex_;
  LockRank rank_;
};

/** A non recursive shared mutex that reports order violations in a debug build. */
class RankedSharedMutex : public rex::NonCopyable {
 public:
  /**
   * @param rank The rank of this lock.
   */
  explicit RankedSharedMutex(LockRank rank) : rank_(rank) {}

  /** Locks the mutex exclusively. */
  void lock() {
    PushLockRank(rank_);
    mutex_.lock();
  }

  /** Unlocks the exclusive lock. */
  void unlock() {
    PopLockRank(rank_);
    mutex_.unlock();
  }

  /** Locks the mutex for shared use. */
  void lock_shared() {
    PushLockRank(rank_);
    mutex_.lock_shared();
  }

  /** Unlocks the shared lock. */
  void unlock_shared() {
    PopLockRank(rank_);
    mutex_.unlock_shared();
  }

 private:
  std::shared_mutex mutex_;
  LockRank rank_;
};

}  // namespace rex::platform
