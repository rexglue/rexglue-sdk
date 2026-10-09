/**
 * @file        runtime/thread_wait.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>
#include <ranges>
#include <span>

#include <rex/net/socket.h>
#include <rex/platform/thread.h>

namespace rex::runtime {

/**
 * Waits on a host object for the calling thread. On a guest thread a stop request wakes the wait
 * and throws ThreadStop, and any other wake waits again for the time left.
 *
 * @param handle The object to wait on.
 * @param timeout The longest wait, milliseconds::max() for no limit.
 * @return The result of the wait.
 */
rex::platform::WaitResult WaitHostObject(rex::platform::WaitHandle* handle,
                                         std::chrono::milliseconds timeout);

/**
 * Waits until 1 socket may be ready, with the stop behavior of WaitHostObject.
 *
 * @param sockets The sockets to watch.
 * @param timeout The longest wait, milliseconds::max() for no limit.
 * @return kSuccess when a socket may be ready, kTimeout or kFailed.
 */
rex::platform::WaitResult WaitHostSockets(std::span<const rex::platform::SocketWait> sockets,
                                          std::chrono::milliseconds timeout);

/**
 * Waits until the host thread of every element ends. Once the timeout passes, it reports the
 * thread every timeout and forces its stop every few milliseconds.
 *
 * @tparam Range A range whose elements dereference to an object with thread() and ForceStop().
 * @tparam Report A callable taking 1 element of the range.
 * @param threads The threads to wait for.
 * @param timeout The time each thread has to stop by itself, and the time between reports.
 * @param report Called with each thread still running when a timeout ends.
 */
template <std::ranges::range Range, typename Report>
void WaitForThreadsToStop(const Range& threads, std::chrono::milliseconds timeout, Report report) {
  constexpr std::chrono::milliseconds kForceStopInterval{5};
  for (const auto& thread : threads) {
    auto wait = timeout;
    std::chrono::milliseconds waited{0};
    while (thread->thread() && rex::platform::Wait(thread->thread(), false, wait) ==
                                   rex::platform::WaitResult::kTimeout) {
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

}  // namespace rex::runtime
