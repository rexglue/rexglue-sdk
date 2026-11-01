/**
 * @file        platform/sync.h
 * @brief       Wait objects and wait functions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/threading.h (Ben Vanik, 2020).
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/platform.h>
#include <rex/types.h>

namespace rex::platform {

/** Event that clears its own signal once the last waiter returns from Wait. */
class Fence {
 public:
  /** Creates an unsignaled fence. */
  Fence() : signal_state_{0} {}

  /** Signals the fence and wakes every waiter. */
  void Signal() {
    std::unique_lock<std::mutex> lock(mutex_);
    signal_state_.store(signal_state_.load() | kSigMask, std::memory_order_release);
    cond_.notify_all();
  }

  /** Blocks until the fence is signaled. The last waiter to return clears the signal. */
  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    REX_ASSERT((signal_state_.load() & ~kSigMask) < (kSigMask - 1) && "Too many threads?");

    auto signal_state = signal_state_.fetch_add(1) + 1;
    for (; !(signal_state & kSigMask); signal_state = signal_state_.load()) {
      cond_.wait(lock);
    }

    REX_ASSERT((signal_state & ~kSigMask) > 0);
    if (signal_state == (1 | kSigMask)) {
      signal_state_.store(0, std::memory_order_release);
    } else {
      signal_state_.store(--signal_state, std::memory_order_release);
    }
  }

 private:
  using StateT = uint_fast32_t;
  static constexpr StateT kSigMask = static_cast<StateT>(1) << (sizeof(StateT) * 8 - 1);

  std::mutex mutex_;
  std::condition_variable cond_;
  std::atomic<StateT> signal_state_;
};

enum class WaitResult : u8 {
  kSuccess,      /**< The object is signaled. WaitAny also reports its index. */
  kUserCallback, /**< A user callback queued to the thread ended an alertable wait. */
  kTimeout,      /**< The timeout elapsed and the object is not signaled. */
  kAbandoned,    /**< A mutant owner exited without a release. The caller owns it now. */
  kFailed,
};

/** Base of every object a thread can wait on. */
class WaitHandle {
 public:
  virtual ~WaitHandle() = default;

  /**
   * Returns the native OS object.
   */
  virtual void* GetNativeHandle() const = 0;

 protected:
  WaitHandle() = default;
};

/**
 * A timeout of 0 polls and max() waits forever.
 *
 * @param is_alertable True to let a queued user callback end the wait.
 */
REX_API WaitResult Wait(WaitHandle* wait_handle, bool is_alertable,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds::max());

/**
 * Signals an object and waits on another as one operation. A timeout of 0 polls and max() waits
 * forever.
 *
 * @param is_alertable True to let a queued user callback end the wait.
 */
REX_API WaitResult SignalAndWait(
    WaitHandle* wait_handle_to_signal, WaitHandle* wait_handle_to_wait_on, bool is_alertable,
    std::chrono::milliseconds timeout = std::chrono::milliseconds::max());

/**
 * Waits on several objects. A timeout of 0 polls and max() waits forever.
 *
 * @param is_alertable True to let a queued user callback end the wait.
 * @return Why the wait ended and, for kSuccess or kAbandoned, the index of the object.
 */
REX_API std::pair<WaitResult, size_t> WaitMultiple(
    WaitHandle* wait_handles[], size_t wait_handle_count, bool wait_all, bool is_alertable,
    std::chrono::milliseconds timeout = std::chrono::milliseconds::max());

/**
 * Waits until every object is signaled. A timeout of 0 polls and max() waits forever.
 *
 * @param is_alertable True to let a queued user callback end the wait.
 */
inline WaitResult WaitAll(WaitHandle* wait_handles[], size_t wait_handle_count, bool is_alertable,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) {
  return WaitMultiple(wait_handles, wait_handle_count, true, is_alertable, timeout).first;
}
/**
 * Waits until every object is signaled. A timeout of 0 polls and max() waits forever.
 *
 * @param is_alertable True to let a queued user callback end the wait.
 */
inline WaitResult WaitAll(std::vector<WaitHandle*> wait_handles, bool is_alertable,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) {
  return WaitAll(wait_handles.data(), wait_handles.size(), is_alertable, timeout);
}

/**
 * Waits until any object is signaled. A timeout of 0 polls and max() waits forever.
 *
 * @param is_alertable True to let a queued user callback end the wait.
 * @return Why the wait ended and the index of the object that satisfied or abandoned it.
 */
inline std::pair<WaitResult, size_t> WaitAny(
    WaitHandle* wait_handles[], size_t wait_handle_count, bool is_alertable,
    std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) {
  return WaitMultiple(wait_handles, wait_handle_count, false, is_alertable, timeout);
}
/**
 * Waits until any object is signaled. A timeout of 0 polls and max() waits forever.
 *
 * @param is_alertable True to let a queued user callback end the wait.
 * @return Why the wait ended and the index of the object that satisfied or abandoned it.
 */
inline std::pair<WaitResult, size_t> WaitAny(
    std::vector<WaitHandle*> wait_handles, bool is_alertable,
    std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) {
  return WaitAny(wait_handles.data(), wait_handles.size(), is_alertable, timeout);
}

/** Event object modeled on the Win32 event. */
class REX_API Event : public WaitHandle {
 public:
  /**
   * Creates an event that stays signaled until Reset.
   *
   * @return The event, or null on failure.
   */
  static std::unique_ptr<Event> CreateManualResetEvent(bool initial_state);

  /**
   * Creates an event that resets after it releases one waiter.
   *
   * @return The event, or null on failure.
   */
  static std::unique_ptr<Event> CreateAutoResetEvent(bool initial_state);

  /** Signals the event. An auto reset event stays signaled until one wait is satisfied. */
  virtual void Set() = 0;

  virtual void Reset() = 0;

  /** Signals the event, releases the waiters it can, then clears the signal. */
  virtual void Pulse() = 0;
};

/** Semaphore object modeled on the Win32 semaphore. */
class REX_API Semaphore : public WaitHandle {
 public:
  /**
   * Creates a semaphore, signaled while its count is above 0.
   *
   * @param initial_count Starting count, from 0 to maximum_count.
   * @return The semaphore, or null on failure.
   */
  static std::unique_ptr<Semaphore> Create(int initial_count, int maximum_count);

  /**
   * Raises the count.
   *
   * @param release_count Amount to add, above 0.
   * @param out_previous_count Receives the previous count, may be null.
   * @return False when the count would pass maximum_count.
   */
  virtual bool Release(int release_count, int* out_previous_count) = 0;
};

/** Mutant object modeled on the Win32 mutex. */
class REX_API Mutant : public WaitHandle {
 public:
  /**
   * Creates a mutant.
   *
   * @param initial_owner True to give ownership to the calling thread.
   * @return The mutant, or null on failure.
   */
  static std::unique_ptr<Mutant> Create(bool initial_owner);

  /**
   * Releases ownership.
   *
   * @return False when the calling thread does not own the mutant.
   */
  virtual bool Release() = 0;
};

}  // namespace rex::platform
