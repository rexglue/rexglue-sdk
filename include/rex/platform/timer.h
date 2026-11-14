/**
 * @file        platform/timer.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/threading.h and xenia/base/threading_timer_queue.h
 *              (Ben Vanik, 2020 and 2022).
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include <rex/assert.h>
#include <rex/chrono.h>
#include <rex/platform.h>
#include <rex/platform/sync.h>
#include <rex/types.h>

namespace rex::platform {

class TimerQueue;

/** Pending timer in the process timer queue. */
struct REX_API TimerQueueWaitItem {
  using clock = std::chrono::steady_clock;

  /**
   * Creates a wait item. Use QueueTimerOnce or QueueTimerRecurring instead.
   *
   * @param callback Function called on the dispatch thread.
   * @param parent_queue Queue that owns the item.
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
   * Cancels the item. No callback runs after it returns. Blocks while the callback runs unless
   * called from that callback. Holding a lock the callback also takes can deadlock.
   */
  void Disarm();

  friend TimerQueue;

 private:
  enum class State : uint_least8_t { kIdle = 0, kInCallback, kInCallbackSelfDisarmed, kDisarmed };
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
 * @return Handle to the queued item, used to Disarm it.
 */
REX_API std::weak_ptr<TimerQueueWaitItem> QueueTimerOnce(std::function<void(void*)> callback,
                                                         void* userdata,
                                                         TimerQueueWaitItem::clock::time_point due);

/**
 * Queues a callback that runs at due and then after each interval.
 *
 * @param callback Function called on the dispatch thread.
 * @param interval Repeat interval, 0 for a single run.
 * @return Handle to the queued item, used to Disarm it.
 */
REX_API std::weak_ptr<TimerQueueWaitItem> QueueTimerRecurring(
    std::function<void(void*)> callback, void* userdata, TimerQueueWaitItem::clock::time_point due,
    TimerQueueWaitItem::clock::duration interval);

/** Repeating millisecond timer whose callback runs on the shared timer queue thread. */
class HighResolutionTimer {
  struct Key {};

 public:
  HighResolutionTimer(Key, std::chrono::milliseconds interval, std::function<void()> callback) {
    REX_ASSERT(callback != nullptr);
    wait_item_ = QueueTimerRecurring([callback = std::move(callback)](void*) { callback(); },
                                     nullptr, TimerQueueWaitItem::clock::now(), interval);
  }

  ~HighResolutionTimer() {
    if (auto wait_item = wait_item_.lock()) {
      wait_item->Disarm();
    }
  }

  static std::unique_ptr<HighResolutionTimer> CreateRepeating(std::chrono::milliseconds period,
                                                              std::function<void()> callback) {
    return std::make_unique<HighResolutionTimer>(Key{}, period, std::move(callback));
  }

 private:
  std::weak_ptr<TimerQueueWaitItem> wait_item_;
};

/**
 * Waitable timer modeled on the Win32 waitable timer. The optional callback runs as a user callback
 * on the thread that set the timer on Windows, and on the timer queue thread on POSIX.
 */
class REX_API Timer : public WaitHandle {
 public:
  using WindowsClock = rex::WinSystemClock;
  using SteadyClock = std::chrono::steady_clock;

  /**
   * Creates a timer that stays signaled until a Set call gives it a new due time.
   *
   * @return The timer, or null on failure.
   */
  static std::unique_ptr<Timer> CreateManualResetTimer();

  /**
   * Creates a timer that resets after a wait on it completes.
   *
   * @return The timer, or null on failure.
   */
  static std::unique_ptr<Timer> CreateSynchronizationTimer();

  /**
   * Arms the timer to fire once after a delay.
   *
   * @param opt_callback Function called when the timer fires, may be null.
   */
  virtual bool SetOnceAfter(rex::HundredNanoseconds rel_time,
                            std::function<void()> opt_callback = nullptr) = 0;
  /**
   * Arms the timer to fire once at a Windows clock time.
   *
   * @param opt_callback Function called when the timer fires, may be null.
   */
  virtual bool SetOnceAt(WindowsClock::time_point due_time,
                         std::function<void()> opt_callback = nullptr) = 0;
  /**
   * Arms the timer to fire once at a steady clock time.
   *
   * @param opt_callback Function called when the timer fires, may be null.
   */
  virtual bool SetOnceAt(SteadyClock::time_point due_time,
                         std::function<void()> opt_callback = nullptr) = 0;

  /**
   * Arms the timer to fire after a delay and then each period until Cancel or a new Set call.
   *
   * @param opt_callback Function called each time the timer fires, may be null.
   */
  virtual bool SetRepeatingAfter(rex::HundredNanoseconds rel_time, std::chrono::milliseconds period,
                                 std::function<void()> opt_callback = nullptr) = 0;
  /**
   * Arms the timer to fire at a Windows clock time and then each period.
   *
   * @param opt_callback Function called each time the timer fires, may be null.
   */
  virtual bool SetRepeatingAt(WindowsClock::time_point due_time, std::chrono::milliseconds period,
                              std::function<void()> opt_callback = nullptr) = 0;
  /**
   * Arms the timer to fire at a steady clock time and then each period.
   *
   * @param opt_callback Function called each time the timer fires, may be null.
   */
  virtual bool SetRepeatingAt(SteadyClock::time_point due_time, std::chrono::milliseconds period,
                              std::function<void()> opt_callback = nullptr) = 0;

  /**
   * Stops the timer and discards pending callbacks. Waiting threads keep waiting and a signaled
   * timer stays signaled.
   */
  virtual bool Cancel() = 0;

  /** Stops the timer, discards pending callbacks and clears the signal. */
  virtual bool Reset() = 0;
};

namespace detail {

/** Destroys the timer queue and joins its thread. The next timer creates it again. */
REX_API void ShutdownTimerQueue();

}  // namespace detail

}  // namespace rex::platform
