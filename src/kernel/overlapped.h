/**
 * @file        kernel/overlapped.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <mutex>

namespace rex::kernel {

/** Returns the lock that orders overlapped completion against its observers. */
std::mutex& GetOverlappedMutex();

/**
 * Runs the writes that complete an overlapped block. An observer sees both the result and the event
 * write, or neither.
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
 * Runs the reads that observe an overlapped block.
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

}  // namespace rex::kernel