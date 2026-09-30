/**
 * @file        system/thread_wait.h
 * @brief       Bounded wait for guest threads to stop running
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <ranges>
#include <thread>

namespace rex::system {

/**
 * Polls until no thread in threads is running or timeout passes.
 *
 * @tparam Range A range whose elements dereference to an object with is_running().
 * @param threads The threads to wait for.
 * @param timeout The bound on the wait.
 * @return True when every thread stopped within the bound.
 */
template <std::ranges::range Range>
bool WaitForThreadsToExit(const Range& threads, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (std::ranges::none_of(threads, [](const auto& thread) { return thread->is_running(); })) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

}  // namespace rex::system
