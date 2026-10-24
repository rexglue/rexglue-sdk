/**
 * @file        runtime/overlapped.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <mutex>

namespace rex::runtime {

/**
 * Returns the lock that orders the completion of an overlapped block against its observers.
 *
 * @return The lock.
 */
std::mutex& GetOverlappedMutex();

/**
 * Runs the writes that complete an overlapped block, its result first and then its event. An
 * observer that reads through ObserveOverlapped sees both writes or neither.
 *
 * @tparam Publish A callable taking no arguments.
 * @param publish Stores the result and sets the event.
 */
template <typename Publish>
void PublishOverlapped(Publish&& publish) {
  std::lock_guard lock(GetOverlappedMutex());
  publish();
}

/**
 * Runs the reads that observe an overlapped block. A result seen as complete has its event set.
 *
 * @tparam Observe A callable taking no arguments.
 * @param observe Reads the result.
 * @return What observe returns.
 */
template <typename Observe>
auto ObserveOverlapped(Observe&& observe) {
  std::lock_guard lock(GetOverlappedMutex());
  return observe();
}

}  // namespace rex::runtime