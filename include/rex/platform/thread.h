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
#include <cstddef>
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

#include <rex/assert.h>
#include <rex/chrono.h>
#include <rex/platform.h>
#include <rex/platform/process.h>
#include <rex/platform/sync.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::platform {

#if REX_PLATFORM_ANDROID
/** Loads the libc symbols thread naming needs on Android API 26 and later. */
REX_API void AndroidInitialize();
/** Releases what AndroidInitialize loaded. */
REX_API void AndroidShutdown();
#endif

REX_API u32 GetLogicalProcessorCount();

/** Widens the process affinity to every processor. Does nothing on POSIX. */
REX_API void WidenProcessAffinity();

/** Returns the OS thread ID of the calling thread. For logs and diagnostics only. */
REX_API u32 GetCallerSystemThreadID();

/**
 * Equals GetCallerSystemThreadID() until SetCallerThreadID overrides it. Guest threads set it to
 * their guest thread handle.
 */
REX_API u32 GetCallerThreadID();

/** Overrides the ID GetCallerThreadID returns on the calling thread. */
REX_API void SetCallerThreadID(u32 id);

/**
 * Names the calling thread for debuggers and OS tools.
 *
 * @param name Thread name, UTF-8.
 */
REX_API void SetCurrentThreadName(const std::string_view name);

/** Yields the calling thread to the scheduler and issues a full memory barrier. */
REX_API void MaybeYield();

/** Issues a full memory barrier. */
REX_API void SyncMemory();

/** Sleeps the calling thread for at least the duration. A duration of 0 yields. */
REX_API void SleepFor(std::chrono::microseconds duration);

template <typename Rep, typename Period>
void SleepFor(std::chrono::duration<Rep, Period> duration) {
  SleepFor(std::chrono::duration_cast<std::chrono::microseconds>(duration));
}

/** Returns the CPU time the calling thread has used. It is kernel plus user time. */
REX_API std::chrono::milliseconds GetCurrentThreadCPUTime();

/** Outcome of AlertableSleep. */
enum class SleepResult : u8 {
  kSuccess,
  kAlerted, /**< A user callback ran and ended the sleep early. */
};

/**
 * Sleeps the calling thread in an alertable state. A queued user callback ends the sleep early.
 *
 * @return kAlerted when a user callback ran, kSuccess otherwise.
 */
REX_API SleepResult AlertableSleep(std::chrono::microseconds duration);

/**
 * Sleeps the calling thread in an alertable state.
 *
 * @return kAlerted when a user callback ran, kSuccess otherwise.
 */
template <typename Rep, typename Period>
SleepResult AlertableSleep(std::chrono::duration<Rep, Period> duration) {
  return AlertableSleep(std::chrono::duration_cast<std::chrono::microseconds>(duration));
}

using TLSHandle = u32;
constexpr TLSHandle kInvalidTLSHandle = UINT_MAX;

REX_API TLSHandle AllocateTLSHandle();

REX_API bool FreeTLSHandle(TLSHandle handle);

/**
 * Reads the value the calling thread holds in a slot.
 *
 * @return Stored value, 0 when never set.
 */
REX_API uintptr_t GetTLSValue(TLSHandle handle);

/** Writes the value the calling thread holds in a slot. */
REX_API bool SetTLSValue(TLSHandle handle, uintptr_t value);

#if REX_PLATFORM_WIN32
/** Thread priority values for Thread::SetPriority. */
struct ThreadPriority {
  static const i32 kLowest = -2;
  static const i32 kBelowNormal = -1;
  static const i32 kNormal = 0;
  static const i32 kAboveNormal = 1;
  static const i32 kHighest = 2;
};
#else
/** Thread priority values for Thread::SetPriority. */
struct ThreadPriority {
  static const i32 kLowest = 1;
  static const i32 kBelowNormal = 8;
  static const i32 kNormal = 16;
  static const i32 kAboveNormal = 24;
  static const i32 kHighest = 32;
};
#endif

/** Thread object modeled on the Win32 thread. */
class REX_API Thread : public WaitHandle {
 public:
  struct CreationParameters {
    size_t stack_size = 4_MiB;
    bool create_suspended = false; /**< Starts suspended until Resume. */
    i32 initial_priority = 0;      /**< Nonzero uses SCHED_FIFO on POSIX, unused on Windows. */
  };

  /**
   * Creates a thread that runs start_routine.
   *
   * @return The thread, or null on failure.
   */
  static std::unique_ptr<Thread> Create(CreationParameters params,
                                        rex::MoveOnlyFunction<void()> start_routine);
  /**
   * Returns the Thread object of the calling thread, wrapping threads Create did not start.
   */
  static Thread* GetCallingThread();

  /**
   * Ends the calling thread without running destructors, signaling the thread object. Does not
   * return.
   */
  static void Exit(int exit_code);

  virtual u32 GetSystemID() const = 0;

  const std::string& GetName() const { return name_; }

  /** Names the thread for debuggers and logs. */
  virtual void SetName(std::string name) { name_ = std::move(name); }

  virtual i32 GetPriority() = 0;

  /**
   * Sets the thread priority.
   *
   * @param new_priority Usually a ThreadPriority value.
   */
  virtual void SetPriority(i32 new_priority) = 0;

  /**
   * Returns the processor affinity mask. The mask has one bit per logical processor the thread may
   * run on.
   */
  virtual u64 GetAffinityMask() = 0;

  /**
   * Sets the processor affinity mask, a subset of the process affinity mask.
   *
   * @param new_affinity_mask 1 bit per logical processor the thread may run on.
   */
  virtual void SetAffinityMask(u64 new_affinity_mask) = 0;

  /**
   * Queues a user callback. Callbacks run in FIFO order when the thread enters an alertable wait,
   * which then returns WaitResult::kUserCallback.
   */
  virtual void QueueUserCallback(rex::MoveOnlyFunction<void()> callback) = 0;

  /**
   * Decrements the suspend count. The thread runs again at 0.
   *
   * @param out_previous_suspend_count Receives the previous count, may be null.
   */
  virtual bool Resume(u32* out_previous_suspend_count = nullptr) = 0;

  /**
   * Increments the suspend count and suspends the thread.
   *
   * @param out_previous_suspend_count Receives the previous count, may be null.
   */
  virtual bool Suspend(u32* out_previous_suspend_count = nullptr) = 0;

  /**
   * Decides whether a stopped thread is acted on.
   *
   * @param pc The instruction pointer of the stopped thread.
   * @param context The context passed to Redirect or SuspendIf.
   */
  using RedirectFilter = bool (*)(u64 pc, void* context);

  /**
   * Makes the thread call target in place of its current instruction when filter accepts it. Filter
   * runs while the thread is stopped, so it must not lock or allocate.
   *
   * @param filter Called with the instruction pointer of the stopped thread.
   * @param target Called on the thread. It never returns.
   * @return True when the thread now runs target.
   */
  virtual bool Redirect(RedirectFilter filter, void* context, void (*target)()) = 0;

  /**
   * Suspends the thread when filter accepts its current instruction. Filter runs while the thread
   * is stopped, so it must not lock or allocate.
   *
   * @param filter Called with the instruction pointer of the stopped thread.
   * @return True when the thread is now suspended and its suspend count went up by 1.
   */
  virtual bool SuspendIf(RedirectFilter filter, void* context) = 0;

 protected:
  std::string name_;
};

namespace detail {

/** Adds the calling thread to the host thread registry until it exits. */
REX_API void RegisterCurrentThread();

/**
 * Counts the live registered threads.
 */
REX_API u64 GetThreadCount();

}  // namespace detail

/**
 * Starts a host thread the SDK tracks until it exits.
 *
 * @tparam F Callable taking a std::stop_token.
 * @param name Thread name shown in debuggers and logs.
 * @return The running thread. Its destructor requests stop and joins.
 */
template <typename F>
std::jthread Spawn(std::string_view name, F&& fn) {
  return std::jthread([thread_name = std::string(name),
                       body = std::forward<F>(fn)](const std::stop_token& token) mutable {
    detail::RegisterCurrentThread();
    PrepareThreadForCrash();
    SetCurrentThreadName(thread_name);
    body(std::move(token));
  });
}

}  // namespace rex::platform

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#if REX_PLATFORM_MAC && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif
#include <ucontext.h>
#include <cstdint>
#include <vector>
#endif

namespace rex::platform {

/**
 * Host OS fiber. Each guest fiber owns one Fiber. A switch keeps the whole C++ call stack.
 */
struct REX_API Fiber {
  /**
   * Converts the calling thread into a fiber.
   *
   * @return Fiber for the current execution context, or null on failure.
   */
  static Fiber* ConvertCurrentThread();

  /**
   * Creates a fiber with its own host stack. The first switch to it calls entry(arg).
   *
   * @return The new fiber, or null on failure.
   */
  static Fiber* Create(size_t stack_size, void (*entry)(void*), void* arg);

  /** Suspends the current fiber and resumes target. Returns when another fiber switches back. */
  static void SwitchTo(Fiber* target);

  /**
   * A created fiber must not be running. A converted thread fiber is destroyed from its own thread
   * and turns it back into a plain thread.
   */
  void Destroy();

  /**
   * Returns the fiber running on the calling thread, or null when the thread was never converted.
   */
  static Fiber* GetCurrent();

 private:
#if REX_PLATFORM_WIN32
  void* handle_ = nullptr;
  bool is_thread_fiber_ = false;
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
  ucontext_t context_{};
  std::vector<u8> stack_;
  void (*entry_)(void*) = nullptr;
  void* arg_ = nullptr;
  bool is_thread_fiber_ = false;

  static void Trampoline();
#endif
};

}  // namespace rex::platform

namespace rex {

/** Unwinds a thread that runs recompiled code to its entry when it stops or exits. */
struct ThreadStop {
  i32 exit_code = 0;
};

}  // namespace rex
