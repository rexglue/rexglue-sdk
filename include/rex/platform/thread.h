/**
 * @file        platform/thread.h
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <rex/macros.h>
#include <rex/assert.h>
#include <rex/chrono/chrono.h>
#include <rex/platform/crash.h>
#include <rex/literals.h>
#include <rex/platform.h>
#include <rex/platform/timer_queue.h>
#include <rex/types.h>
#include <rex/move_only_function.h>

namespace rex::platform {

#if REX_PLATFORM_ANDROID
/** Loads the libc symbols thread naming needs on Android API 26 and later. */
REX_API void AndroidInitialize();
/** Releases what AndroidInitialize loaded. */
REX_API void AndroidShutdown();
#endif

/** Event that clears its own signal once the last waiter returns from Wait. */
class Fence {
 public:
  /** Creates an unsignaled fence. */
  Fence() : signal_state_{0} {}

  /** Signals the fence and wakes every waiter. */
  void Signal() {
    std::unique_lock<std::mutex> lock(mutex_);
    signal_state_.store(signal_state_.load() | SIGMASK_, std::memory_order_release);
    cond_.notify_all();
  }

  /** Blocks until the fence is signaled. The last waiter to return clears the signal. */
  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    REX_ASSERT((signal_state_.load() & ~SIGMASK_) < (SIGMASK_ - 1) && "Too many threads?");

    auto signal_state = signal_state_.fetch_add(1) + 1;
    for (; !(signal_state & SIGMASK_); signal_state = signal_state_.load()) {
      cond_.wait(lock);
    }

    REX_ASSERT((signal_state & ~SIGMASK_) > 0);
    if (signal_state == (1 | SIGMASK_)) {
      signal_state_.store(0, std::memory_order_release);
    } else {
      signal_state_.store(--signal_state, std::memory_order_release);
    }
  }

 private:
  using state_t_ = uint_fast32_t;
  static constexpr state_t_ SIGMASK_ = state_t_(1) << (sizeof(state_t_) * 8 - 1);

  std::mutex mutex_;
  std::condition_variable cond_;
  std::atomic<state_t_> signal_state_;
};

/**
 * Counts the logical processors of the host.
 *
 * @return Logical processor count.
 */
REX_API u32 logical_processor_count();

/**
 * Widens the process affinity to every processor so threads can pin to any of them. Call at
 * startup before setting thread affinity. Does nothing on POSIX.
 */
REX_API void EnableAffinityConfiguration();

/**
 * Returns the OS thread ID of the calling thread. For logs and diagnostics only.
 *
 * @return OS thread ID.
 */
REX_API u32 current_thread_system_id();

/**
 * Returns the ID of the calling thread. Equals current_thread_system_id() until
 * set_current_thread_id overrides it. Guest threads set it to their guest thread handle.
 *
 * @return Thread ID.
 */
REX_API u32 current_thread_id();

/**
 * Overrides the ID current_thread_id returns on the calling thread.
 *
 * @param id New thread ID.
 */
REX_API void set_current_thread_id(u32 id);

/**
 * Names the calling thread for debuggers and OS tools.
 *
 * @param name Thread name, UTF-8.
 */
REX_API void set_current_thread_name(const std::string_view name);

/** Yields the calling thread to the scheduler and issues a full memory barrier. */
REX_API void MaybeYield();

/** Issues a full memory barrier. */
REX_API void SyncMemory();

/**
 * Sleeps the calling thread for at least the duration. On Windows a high resolution waitable timer
 * covers all but the last 250 microseconds, which spin on the steady clock. A duration of 0 yields.
 *
 * @param duration Sleep time.
 */
REX_API void Sleep(std::chrono::microseconds duration);

/**
 * Sleeps the calling thread for at least the duration.
 *
 * @tparam Rep Duration count type.
 * @tparam Period Duration tick period.
 * @param duration Sleep time.
 */
template <typename Rep, typename Period>
void Sleep(std::chrono::duration<Rep, Period> duration) {
  Sleep(std::chrono::duration_cast<std::chrono::microseconds>(duration));
}

/**
 * Returns the CPU time the calling thread has used.
 *
 * @return Kernel plus user time.
 */
REX_API std::chrono::milliseconds QueryCurrentThreadCPUTime();

/** Outcome of AlertableSleep. */
enum class SleepResult : u8 {
  kSuccess, /**< The full duration elapsed. */
  kAlerted, /**< A user callback ran and ended the sleep early. */
};

/**
 * Sleeps the calling thread in an alertable state. Queued user callbacks run during the sleep
 * and end it early.
 *
 * @param duration Sleep time.
 * @return kAlerted when a user callback ran, kSuccess otherwise.
 */
REX_API SleepResult AlertableSleep(std::chrono::microseconds duration);

/**
 * Sleeps the calling thread in an alertable state.
 *
 * @tparam Rep Duration count type.
 * @tparam Period Duration tick period.
 * @param duration Sleep time.
 * @return kAlerted when a user callback ran, kSuccess otherwise.
 */
template <typename Rep, typename Period>
SleepResult AlertableSleep(std::chrono::duration<Rep, Period> duration) {
  return AlertableSleep(std::chrono::duration_cast<std::chrono::microseconds>(duration));
}

/** Host thread local storage slot. */
using TLSHandle = u32;
/** TLSHandle value that names no slot. */
constexpr TLSHandle kInvalidTLSHandle = UINT_MAX;

/**
 * Allocates a thread local storage slot.
 *
 * @return The new slot.
 */
REX_API TLSHandle AllocateTLSHandle();

/**
 * Frees a thread local storage slot.
 *
 * @param handle Slot to free.
 * @return True on success.
 */
REX_API bool FreeTLSHandle(TLSHandle handle);

/**
 * Reads the value the calling thread holds in a slot.
 *
 * @param handle Slot to read.
 * @return Stored value, 0 when never set.
 */
REX_API uintptr_t GetTLSValue(TLSHandle handle);

/**
 * Writes the value the calling thread holds in a slot.
 *
 * @param handle Slot to write.
 * @param value Value to store.
 * @return True on success.
 */
REX_API bool SetTLSValue(TLSHandle handle, uintptr_t value);

/**
 * Repeating millisecond timer on the shared timer queue thread. Callbacks of every timer run
 * on that thread, so keep them short. Destruction cancels the timer.
 */
class HighResolutionTimer {
  /** Limits construction to the Create functions. */
  struct Key {};

 public:
  /**
   * Starts the timer.
   *
   * @param key The private construction key.
   * @param interval Period.
   * @param callback Function called each period.
   */
  HighResolutionTimer(Key, std::chrono::milliseconds interval, std::function<void()> callback) {
    REX_ASSERT(callback != nullptr);
    wait_item_ = QueueTimerRecurring([callback = std::move(callback)](void*) { callback(); },
                                     nullptr, TimerQueueWaitItem::clock::now(), interval);
  }

  /** Stops the timer. */
  ~HighResolutionTimer() {
    if (auto wait_item = wait_item_.lock()) {
      wait_item->Disarm();
    }
  }

  /**
   * Creates a repeating timer.
   *
   * @param period Period.
   * @param callback Function called as close to each period as possible.
   * @return The running timer.
   */
  static std::unique_ptr<HighResolutionTimer> CreateRepeating(std::chrono::milliseconds period,
                                                              std::function<void()> callback) {
    return std::make_unique<HighResolutionTimer>(Key{}, period, std::move(callback));
  }

 private:
  std::weak_ptr<TimerQueueWaitItem> wait_item_;
};

/** Result of a wait operation. */
enum class WaitResult : u8 {
  kSuccess,      /**< The object is signaled. WaitAny also reports its index. */
  kUserCallback, /**< A user callback queued to the thread ended an alertable wait. */
  kTimeout,      /**< The timeout elapsed and the object is not signaled. */
  kAbandoned,    /**< A mutant owner exited without a release. The caller owns it now. */
  kFailed,       /**< The wait failed. */
};

/** Base of every object a thread can wait on. */
class WaitHandle {
 public:
  virtual ~WaitHandle() = default;

  /**
   * Returns the native OS object.
   *
   * @return Platform specific handle.
   */
  virtual void* native_handle() const = 0;

 protected:
  WaitHandle() = default;
};

/**
 * Waits until the handle is signaled, a user callback runs during an alertable wait, or the
 * timeout elapses. A timeout of 0 polls and max() waits forever.
 *
 * @param wait_handle Object to wait on.
 * @param is_alertable True to let queued user callbacks run and end the wait.
 * @param timeout Maximum wait.
 * @return Why the wait ended.
 */
REX_API WaitResult Wait(WaitHandle* wait_handle, bool is_alertable,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds::max());

/**
 * Signals 1 object and waits on another as a single operation. A timeout of 0 polls and max()
 * waits forever.
 *
 * @param wait_handle_to_signal Object to signal.
 * @param wait_handle_to_wait_on Object to wait on.
 * @param is_alertable True to let queued user callbacks run and end the wait.
 * @param timeout Maximum wait.
 * @return Why the wait ended.
 */
REX_API WaitResult SignalAndWait(
    WaitHandle* wait_handle_to_signal, WaitHandle* wait_handle_to_wait_on, bool is_alertable,
    std::chrono::milliseconds timeout = std::chrono::milliseconds::max());

/**
 * Waits on several objects. A timeout of 0 polls and max() waits forever.
 *
 * @param wait_handles Objects to wait on.
 * @param wait_handle_count Number of objects.
 * @param wait_all True to wait for every object, false for any.
 * @param is_alertable True to let queued user callbacks run and end the wait.
 * @param timeout Maximum wait.
 * @return Why the wait ended and, for kSuccess or kAbandoned, the index of the object.
 */
REX_API std::pair<WaitResult, size_t> WaitMultiple(
    WaitHandle* wait_handles[], size_t wait_handle_count, bool wait_all, bool is_alertable,
    std::chrono::milliseconds timeout = std::chrono::milliseconds::max());

/**
 * Waits until every object is signaled, a user callback runs during an alertable wait, or the
 * timeout elapses. A timeout of 0 polls and max() waits forever.
 *
 * @param wait_handles Objects to wait on.
 * @param wait_handle_count Number of objects.
 * @param is_alertable True to let queued user callbacks run and end the wait.
 * @param timeout Maximum wait.
 * @return Why the wait ended.
 */
inline WaitResult WaitAll(WaitHandle* wait_handles[], size_t wait_handle_count, bool is_alertable,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) {
  return WaitMultiple(wait_handles, wait_handle_count, true, is_alertable, timeout).first;
}
/**
 * Waits until every object is signaled, a user callback runs during an alertable wait, or the
 * timeout elapses.
 *
 * @param wait_handles Objects to wait on.
 * @param is_alertable True to let queued user callbacks run and end the wait.
 * @param timeout Maximum wait.
 * @return Why the wait ended.
 */
inline WaitResult WaitAll(std::vector<WaitHandle*> wait_handles, bool is_alertable,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) {
  return WaitAll(wait_handles.data(), wait_handles.size(), is_alertable, timeout);
}

/**
 * Waits until any object is signaled, a user callback runs during an alertable wait, or the
 * timeout elapses. A timeout of 0 polls and max() waits forever.
 *
 * @param wait_handles Objects to wait on.
 * @param wait_handle_count Number of objects.
 * @param is_alertable True to let queued user callbacks run and end the wait.
 * @param timeout Maximum wait.
 * @return Why the wait ended and the index of the object that satisfied or abandoned it.
 */
inline std::pair<WaitResult, size_t> WaitAny(
    WaitHandle* wait_handles[], size_t wait_handle_count, bool is_alertable,
    std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) {
  return WaitMultiple(wait_handles, wait_handle_count, false, is_alertable, timeout);
}
/**
 * Waits until any object is signaled, a user callback runs during an alertable wait, or the
 * timeout elapses.
 *
 * @param wait_handles Objects to wait on.
 * @param is_alertable True to let queued user callbacks run and end the wait.
 * @param timeout Maximum wait.
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
   * @param initial_state True to start signaled.
   * @return The event, or null on failure.
   */
  static std::unique_ptr<Event> CreateManualResetEvent(bool initial_state);

  /**
   * Creates an event that resets after it releases 1 waiter.
   *
   * @param initial_state True to start signaled.
   * @return The event, or null on failure.
   */
  static std::unique_ptr<Event> CreateAutoResetEvent(bool initial_state);

  /**
   * Signals the event. A manual reset event stays signaled until Reset. An auto reset event
   * stays signaled until 1 wait is satisfied.
   */
  virtual void Set() = 0;

  /** Clears the signal. No effect when already clear. */
  virtual void Reset() = 0;

  /** Signals the event, releases the waiters it can, then clears the signal. */
  virtual void Pulse() = 0;
};

/** Semaphore object modeled on the Win32 semaphore. */
class REX_API Semaphore : public WaitHandle {
 public:
  /**
   * Creates a semaphore. It is signaled while its count is above 0. Each released waiter
   * lowers the count by 1.
   *
   * @param initial_count Starting count, from 0 to maximum_count.
   * @param maximum_count Highest count allowed.
   * @return The semaphore, or null on failure.
   */
  static std::unique_ptr<Semaphore> Create(int initial_count, int maximum_count);

  /**
   * Raises the count.
   *
   * @param release_count Amount to add, above 0.
   * @param out_previous_count Receives the count before the call, may be null.
   * @return False when the new count would pass maximum_count.
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

/**
 * Waitable timer object modeled on the Win32 waitable timer. On Windows the optional callback
 * runs as a user callback on the thread that set the timer. On POSIX it runs on the timer
 * queue thread.
 */
class REX_API Timer : public WaitHandle {
 public:
  /** Clock of Windows kernel sync primitives. */
  using WClock_ = rex::WinSystemClock;
  /** Clock of the SDK timer queue. */
  using GClock_ = std::chrono::steady_clock;

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
   * @param rel_time Delay from now.
   * @param opt_callback Function called when the timer fires, may be null.
   * @return True on success.
   */
  virtual bool SetOnceAfter(rex::hundrednanoseconds rel_time,
                            std::function<void()> opt_callback = nullptr) = 0;
  /**
   * Arms the timer to fire once at a Windows clock time.
   *
   * @param due_time Due time.
   * @param opt_callback Function called when the timer fires, may be null.
   * @return True on success.
   */
  virtual bool SetOnceAt(WClock_::time_point due_time,
                         std::function<void()> opt_callback = nullptr) = 0;
  /**
   * Arms the timer to fire once at a steady clock time.
   *
   * @param due_time Due time.
   * @param opt_callback Function called when the timer fires, may be null.
   * @return True on success.
   */
  virtual bool SetOnceAt(GClock_::time_point due_time,
                         std::function<void()> opt_callback = nullptr) = 0;

  /**
   * Arms the timer to fire after a delay and then each period until Cancel or a new Set call.
   *
   * @param rel_time Delay from now.
   * @param period Repeat period.
   * @param opt_callback Function called each time the timer fires, may be null.
   * @return True on success.
   */
  virtual bool SetRepeatingAfter(rex::hundrednanoseconds rel_time, std::chrono::milliseconds period,
                                 std::function<void()> opt_callback = nullptr) = 0;
  /**
   * Arms the timer to fire at a Windows clock time and then each period.
   *
   * @param due_time First due time.
   * @param period Repeat period.
   * @param opt_callback Function called each time the timer fires, may be null.
   * @return True on success.
   */
  virtual bool SetRepeatingAt(WClock_::time_point due_time, std::chrono::milliseconds period,
                              std::function<void()> opt_callback = nullptr) = 0;
  /**
   * Arms the timer to fire at a steady clock time and then each period.
   *
   * @param due_time First due time.
   * @param period Repeat period.
   * @param opt_callback Function called each time the timer fires, may be null.
   * @return True on success.
   */
  virtual bool SetRepeatingAt(GClock_::time_point due_time, std::chrono::milliseconds period,
                              std::function<void()> opt_callback = nullptr) = 0;

  /**
   * Stops the timer before it fires and drops pending callbacks. Threads waiting on it keep
   * waiting. A signaled timer stays signaled.
   *
   * @return True on success.
   */
  virtual bool Cancel() = 0;

  /**
   * Stops the timer, drops pending callbacks and resets it to non signaled.
   *
   * @return True on success.
   */
  virtual bool Reset() = 0;
};

#if REX_PLATFORM_WIN32
/** Thread priority values for Thread::set_priority. */
struct ThreadPriority {
  static const i32 kLowest = -2;      /**< Lowest priority. */
  static const i32 kBelowNormal = -1; /**< Below normal priority. */
  static const i32 kNormal = 0;       /**< Normal priority. */
  static const i32 kAboveNormal = 1;  /**< Above normal priority. */
  static const i32 kHighest = 2;      /**< Highest priority. */
};
#else
/** Thread priority values for Thread::set_priority. */
struct ThreadPriority {
  static const i32 kLowest = 1;       /**< Lowest priority. */
  static const i32 kBelowNormal = 8;  /**< Below normal priority. */
  static const i32 kNormal = 16;      /**< Normal priority. */
  static const i32 kAboveNormal = 24; /**< Above normal priority. */
  static const i32 kHighest = 32;     /**< Highest priority. */
};
#endif

/** Thread object modeled on the Win32 thread. */
class REX_API Thread : public WaitHandle {
 public:
  /** Options for Create. */
  struct CreationParameters {
    size_t stack_size = 4_MiB;     /**< Stack size in bytes. */
    bool create_suspended = false; /**< True to start suspended until Resume. */
    i32 initial_priority = 0;      /**< Nonzero uses SCHED_FIFO on POSIX. Unused on Windows. */
  };

  /**
   * Creates a thread that runs start_routine.
   *
   * @param params Creation options.
   * @param start_routine Function the thread runs.
   * @return The thread, or null on failure.
   */
  static std::unique_ptr<Thread> Create(CreationParameters params,
                                        rex::MoveOnlyFunction<void()> start_routine);
  /**
   * Returns the Thread object of the calling thread, wrapping threads that Create did not start.
   *
   * @return The calling thread.
   */
  static Thread* GetCurrentThread();

  /**
   * Ends the calling thread without running destructors. Does not return. Signals the thread
   * object and releases its waiters.
   *
   * @param exit_code Thread exit code.
   */
  static void Exit(int exit_code);

  /**
   * Returns the OS thread ID.
   *
   * @return OS thread ID.
   */
  virtual u32 system_id() const = 0;

  /**
   * Returns the thread name.
   *
   * @return Name set by set_name, empty when never set.
   */
  const std::string& name() const { return name_; }

  /**
   * Names the thread for debuggers and logs.
   *
   * @param name Thread name.
   */
  virtual void set_name(std::string name) { name_ = std::move(name); }

  /**
   * Returns the thread priority.
   *
   * @return Current priority.
   */
  virtual i32 priority() = 0;

  /**
   * Sets the thread priority, combined with the process priority class into the base priority.
   *
   * @param new_priority New priority, usually a ThreadPriority value.
   */
  virtual void set_priority(i32 new_priority) = 0;

  /**
   * Returns the processor affinity mask.
   *
   * @return 1 bit per logical processor the thread may run on.
   */
  virtual u64 affinity_mask() = 0;

  /**
   * Sets the processor affinity mask. It must be a subset of the process affinity mask.
   *
   * @param new_affinity_mask 1 bit per logical processor the thread may run on.
   */
  virtual void set_affinity_mask(u64 new_affinity_mask) = 0;

  /**
   * Queues a user callback to the thread. Callbacks run in FIFO order once the thread enters an
   * alertable wait, which then returns WaitResult::kUserCallback.
   *
   * @param callback Function to run on the thread.
   */
  virtual void QueueUserCallback(rex::MoveOnlyFunction<void()> callback) = 0;

  /**
   * Decrements the suspend count. The thread runs again when it reaches 0.
   *
   * @param out_previous_suspend_count Receives the count before the call, may be null.
   * @return True on success.
   */
  virtual bool Resume(u32* out_previous_suspend_count = nullptr) = 0;

  /**
   * Increments the suspend count and suspends the thread.
   *
   * @param out_previous_suspend_count Receives the count before the call, may be null.
   * @return True on success.
   */
  virtual bool Suspend(u32* out_previous_suspend_count = nullptr) = 0;

  /**
   * Decides whether Redirect sends a stopped thread to its target.
   *
   * @param pc The instruction pointer of the stopped thread.
   * @param context The context passed to Redirect.
   * @return True to send the thread to the target.
   */
  using RedirectFilter = bool (*)(u64 pc, void* context);

  /**
   * Makes the thread call target in place of its current instruction when filter accepts that
   * instruction. Filter runs while the thread does not run, so it must not lock or allocate.
   *
   * @param filter Called with the instruction pointer of the stopped thread.
   * @param context Passed to filter.
   * @param target Called on the thread. It never returns.
   * @return True when the thread now runs target.
   */
  virtual bool Redirect(RedirectFilter filter, void* context, void (*target)()) = 0;

  /**
   * Suspends the thread when filter accepts its current instruction, and leaves it running
   * otherwise. Filter runs while the thread does not run, so it must not lock or allocate.
   *
   * @param filter Called with the instruction pointer of the stopped thread.
   * @param context Passed to filter.
   * @return True when the thread is now suspended and its suspend count went up by 1.
   */
  virtual bool SuspendIf(RedirectFilter filter, void* context) = 0;

 protected:
  std::string name_; /**< Thread name returned by name() and replaced by set_name(). */
};

namespace detail {

/** Adds the calling thread to the SDK host thread registry until the thread exits. */
REX_API void RegisterCurrentThread();

/**
 * Counts the SDK host threads that are alive.
 *
 * @return The number of registered threads.
 */
REX_API u64 ThreadCount();

/** Destroys the timer queue and joins its thread. The next timer creates it again. */
REX_API void ShutdownTimerQueue();

}  // namespace detail

/**
 * Starts a host thread the SDK tracks until it exits.
 *
 * @tparam F Callable taking a std::stop_token.
 * @param name Thread name shown in debuggers and logs.
 * @param fn Function the thread runs.
 * @return The running thread. Its destructor requests stop and joins.
 */
template <typename F>
std::jthread Spawn(std::string_view name, F&& fn) {
  return std::jthread([thread_name = std::string(name),
                       body = std::forward<F>(fn)](const std::stop_token& token) mutable {
    detail::RegisterCurrentThread();
    PrepareThreadForCrash();
    set_current_thread_name(thread_name);
    body(std::move(token));
  });
}

}  // namespace rex::platform
