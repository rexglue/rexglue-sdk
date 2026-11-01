/**
 * @file        platform/lock_order.h
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

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::platform {

/**
 * A lock may be taken only when its rank is greater than every rank the thread holds. Modules
 * define lower ranks as values of this type, for example `LockRank kObjectTable{3};`.
 */
enum class LockRank : u8 {
  kLeaf = 0xFF, /**< The highest rank. Nothing may be taken while it is held. */
};

/**
 * Receives an order violation in a debug build.
 *
 * @param held The rank on top of the held stack.
 */
using LockOrderHandler = void (*)(LockRank held, LockRank acquiring);

/**
 * Replaces the violation handler. The default handler asserts.
 *
 * @param handler The new handler, or null to restore the default.
 * @return The previous handler.
 */
REX_API LockOrderHandler SetLockOrderHandler(LockOrderHandler handler);

/** Pushes the rank of a lock being taken and reports an order violation in a debug build. */
REX_API void PushLockRank(LockRank rank);

REX_API void PopLockRank(LockRank rank);

/** A non recursive mutex that reports order violations in a debug build. */
class RankedMutex : public rex::NonCopyable {
 public:
  explicit RankedMutex(LockRank rank) : rank_(rank) {}

  void lock() {
    PushLockRank(rank_);
    mutex_.lock();
  }

  bool try_lock() {
    if (!mutex_.try_lock()) {
      return false;
    }
    PushLockRank(rank_);
    return true;
  }

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
  explicit RankedSharedMutex(LockRank rank) : rank_(rank) {}

  void lock() {
    PushLockRank(rank_);
    mutex_.lock();
  }

  void unlock() {
    PopLockRank(rank_);
    mutex_.unlock();
  }

  void lock_shared() {
    PushLockRank(rank_);
    mutex_.lock_shared();
  }

  void unlock_shared() {
    PopLockRank(rank_);
    mutex_.unlock_shared();
  }

 private:
  std::shared_mutex mutex_;
  LockRank rank_;
};

}  // namespace rex::platform

namespace rex::platform::lock_rank {

/** Rank 1, the lowest. Guards the interrupt state of the function dispatcher. */
inline constexpr LockRank kInterruptState{1};

/** Rank 2. Guards the identity map of the object table and the module load counts. */
inline constexpr LockRank kObjectIdentityMap{2};

/** Rank 3. Guards the object table. */
inline constexpr LockRank kObjectTable{3};

/** Rank 4. Guards the name map of the object table. */
inline constexpr LockRank kObjectNameMap{4};

/** Rank 5. Guards the thread registry. */
inline constexpr LockRank kThreadRegistry{5};

/** Rank 6. Guards the module registry. */
inline constexpr LockRank kModuleRegistry{6};

/** Rank 7. Guards the state of a single object, such as the APU. */
inline constexpr LockRank kObjectState{7};

/** Rank 8. Guards the deferred procedure call queue of the dispatcher. */
inline constexpr LockRank kDPC{8};

}  // namespace rex::platform::lock_rank
