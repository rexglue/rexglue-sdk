/**
 * @file        platform/mutex.h
 * @brief       Global critical region lock
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/mutex.h (Ben Vanik, 2015).
 */

#pragma once

#include <mutex>

#include <rex/macros.h>

namespace rex::platform {

/**
 * Process wide recursive lock that guards operations which suspend threads or must not be
 * interrupted by a suspend, such as global table updates. Acquire it before any other lock and
 * hold it for the whole region. Code reached only from guest threads needs no other lock. Code
 * also reached from host threads may need more.
 *
 * Holding the region is like disabling interrupts on the Xbox 360. The holder cannot be
 * suspended by another thread that also takes the region, so work inside it must be short
 * and do no IO. Keep an instance next to the data it guards to show that the region
 * protects that data.
 *
 * @code
 * class Table {
 *   void Insert(Entry entry) {
 *     auto global_lock = global_critical_region_.Acquire();
 *     entries_.push_back(entry);
 *   }
 *
 *   global_critical_region global_critical_region_;
 *   std::vector<Entry> entries_;
 * };
 * @endcode
 */
class REX_API global_critical_region {
 public:
  /**
   * Returns the process wide mutex.
   *
   * @return The mutex behind every global_critical_region.
   */
  static std::recursive_mutex& mutex();

  /**
   * Locks the region without an instance. Prefer an instance next to the guarded members.
   *
   * @return Owning lock.
   */
  static std::unique_lock<std::recursive_mutex> AcquireDirect() {
    return std::unique_lock<std::recursive_mutex>(mutex());
  }

  /**
   * Locks the region.
   *
   * @return Owning lock.
   */
  inline std::unique_lock<std::recursive_mutex> Acquire() {
    return std::unique_lock<std::recursive_mutex>(mutex());
  }

  /**
   * Creates a lock on the region without locking it.
   *
   * @return Deferred lock.
   */
  inline std::unique_lock<std::recursive_mutex> AcquireDeferred() {
    return std::unique_lock<std::recursive_mutex>(mutex(), std::defer_lock);
  }

  /**
   * Tries to lock the region without blocking.
   *
   * @return Lock whose owns_lock() tells whether the region was taken.
   */
  inline std::unique_lock<std::recursive_mutex> TryAcquire() {
    return std::unique_lock<std::recursive_mutex>(mutex(), std::try_to_lock);
  }
};

}  // namespace rex::platform
