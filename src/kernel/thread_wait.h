/**
 * @file        kernel/thread_wait.h
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
#include <span>

#include <rex/platform/socket.h>
#include <rex/platform/thread.h>
#include <rex/platform/sync.h>

#include <rex/platform.h>

namespace rex::kernel {

/**
 * Serves UI work for a wait on a thread that may call IUISystem::RunSynchronous.
 *
 * @param timeout The longest wait for new work.
 * @return False when this is not the UI thread or no title runtime has the UI, and nothing ran.
 */
bool ServeUI(std::chrono::milliseconds timeout);

/**
 * On a guest thread a stop request wakes the wait and throws ThreadStop. Any other wake waits
 * again for the time left.
 *
 * @param timeout The longest wait, milliseconds::max() for no limit.
 */
rex::platform::WaitResult WaitHostObject(rex::platform::WaitHandle* handle,
                                         std::chrono::milliseconds timeout);

/**
 * Waits until a socket may be ready, with the stop behavior of WaitHostObject.
 *
 * @param sockets The sockets to watch.
 * @param timeout The longest wait, milliseconds::max() for no limit.
 * @return kSuccess when a socket may be ready, kTimeout or kFailed.
 */
rex::platform::WaitResult WaitHostSockets(std::span<const rex::platform::SocketWait> sockets,
                                          std::chrono::milliseconds timeout);

/**
 * On the UI thread it runs the posted UI work between short waits.
 *
 * @param timeout The longest wait.
 */
inline rex::platform::WaitResult WaitThreadServingUI(rex::platform::WaitHandle* handle,
                                                     std::chrono::milliseconds timeout) {
  constexpr std::chrono::milliseconds kWaitStep{2};
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    const auto result = rex::platform::Wait(handle, false, std::chrono::milliseconds{0});
    if (result != rex::platform::WaitResult::kTimeout) {
      return result;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
      return rex::platform::WaitResult::kTimeout;
    }
    if (!ServeUI(std::min(kWaitStep, left))) {
      return rex::platform::Wait(handle, false, left);
    }
  }
}

/**
 * Waits until the host thread of every element ends. Once the timeout passes, it reports the
 * thread every timeout and forces its stop every 5 ms.
 *
 * @tparam Range A range whose elements dereference to an object with GetThread() and ForceStop().
 * @tparam Report A callable taking an element of the range.
 * @param threads The threads to wait for.
 * @param timeout The time each thread has to stop by itself, and the time between reports.
 * @param report Called with each thread that runs when a timeout ends.
 */
template <std::ranges::range Range, typename Report>
void WaitForThreadsToStop(const Range& threads, std::chrono::milliseconds timeout, Report report) {
  constexpr std::chrono::milliseconds kForceStopInterval{5};
  for (const auto& thread : threads) {
    auto wait = timeout;
    std::chrono::milliseconds waited{0};
    while (thread->GetThread() &&
           WaitThreadServingUI(thread->GetThread(), wait) == rex::platform::WaitResult::kTimeout) {
      waited += wait;
      if (waited >= timeout) {
        report(thread);
        waited = {};
      }
      thread->ForceStop();
      wait = kForceStopInterval;
    }
  }
}

}  // namespace rex::kernel
