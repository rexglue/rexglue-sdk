/**
 * @file        platform/timer_queue.h
 * @brief       Timer queue that runs callbacks on 1 dispatch thread
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/threading_timer_queue.h (Ben Vanik, 2022).
 */

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

#include <rex/macros.h>

namespace rex::platform {

class TimerQueue;

/**
 * Pending timer in the process timer queue, like a Windows timer queue timer created with
 * WT_EXECUTEINTIMERTHREAD.
 */
struct REX_API TimerQueueWaitItem {
  /** Clock for due times and intervals. */
  using clock = std::chrono::steady_clock;

  /**
   * Creates a wait item. QueueTimerOnce and QueueTimerRecurring create and queue items.
   *
   * @param callback Function called on the dispatch thread.
   * @param userdata Argument passed to callback.
   * @param parent_queue Queue that owns the item.
   * @param due First due time.
   * @param interval Repeat interval, 0 for a single run.
   */
  TimerQueueWaitItem(std::function<void(void*)> callback, void* userdata, TimerQueue* parent_queue,
                     clock::time_point due, clock::duration interval)
      : callback_(std::move(callback)),
        userdata_(userdata),
        parent_queue_(parent_queue),
        due_(due),
        interval_(interval),
        state_(State::kIdle) {}

  /**
   * Cancels the item. No callback runs after it returns. Blocks while the callback runs,
   * except when called from that callback, where it marks the item and returns at once.
   * Holding a lock the running callback also takes can deadlock.
   */
  void Disarm();

  friend TimerQueue;

 private:
  enum class State : uint_least8_t {
    kIdle = 0,               /**< Waiting for the due time. */
    kInCallback,             /**< Callback running. */
    kInCallbackSelfDisarmed, /**< Callback running and disarmed itself. */
    kDisarmed                /**< Disarmed, waiting for destruction. */
  };
  static_assert(std::atomic<State>::is_always_lock_free);

  std::function<void(void*)> callback_;
  void* userdata_;
  TimerQueue* parent_queue_;
  clock::time_point due_;
  clock::duration interval_;
  std::atomic<State> state_;
};

/**
 * Queues a callback that runs once.
 *
 * @param callback Function called on the dispatch thread.
 * @param userdata Argument passed to callback.
 * @param due Due time.
 * @return Handle to the queued item, used to Disarm it.
 */
REX_API std::weak_ptr<TimerQueueWaitItem> QueueTimerOnce(std::function<void(void*)> callback,
                                                         void* userdata,
                                                         TimerQueueWaitItem::clock::time_point due);

/**
 * Queues a callback that runs at due and then after each interval. The first run is scheduled
 * at max(now - interval, due) to limit a burst of late callbacks.
 *
 * @param callback Function called on the dispatch thread.
 * @param userdata Argument passed to callback.
 * @param due First due time.
 * @param interval Repeat interval, 0 for a single run.
 * @return Handle to the queued item, used to Disarm it.
 */
REX_API std::weak_ptr<TimerQueueWaitItem> QueueTimerRecurring(
    std::function<void(void*)> callback, void* userdata, TimerQueueWaitItem::clock::time_point due,
    TimerQueueWaitItem::clock::duration interval);

}  // namespace rex::platform
