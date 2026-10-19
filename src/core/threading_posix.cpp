/**
 * @file        core/threading_posix.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/net/socket.h>
#include <rex/platform.h>
#include <rex/platform/thread.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <signal.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <poll.h>
#include <pthread.h>
#include <semaphore.h>

#include <fcntl.h>
#if defined(__APPLE__)
#include <cstdio>

#include <mach/mach.h>
#else
#include <sys/eventfd.h>
#endif
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <rex/assert.h>
#include <rex/chrono/chrono_steady_cast.h>
#include <rex/logging/log.h>
#include <rex/platform/timer_queue.h>

#include "core/signal_context.h"

#include <sched.h>

#if REX_PLATFORM_ANDROID
#include <dlfcn.h>

#include <rex/main_android.h>
#include <rex/string.h>
#include <rex/move_only_function.h>
#endif

#if REX_PLATFORM_MAC
static_assert(rex::platform::detail::kRedirectFrameFP == 0x00 &&
              rex::platform::detail::kRedirectFrameLR == 0x08 &&
              rex::platform::detail::kRedirectFrameSP == 0x10 &&
              rex::platform::detail::kRedirectFramePC == 0x18 &&
              rex::platform::detail::kRedirectFrameTarget == 0x20);
#if REX_ARCH_AMD64
__asm__(
    ".text\n"
    ".globl _rex_redirect_trampoline\n"
    ".private_extern _rex_redirect_trampoline\n"
    ".p2align 4\n"
    "_rex_redirect_trampoline:\n"
    ".cfi_startproc\n"
    ".cfi_signal_frame\n"
    ".cfi_escape 0x0f, 0x03, 0x77, 0x10, 0x06\n"
    ".cfi_escape 0x10, 0x06, 0x02, 0x77, 0x00\n"
    ".cfi_escape 0x10, 0x10, 0x02, 0x77, 0x18\n"
    "  call *0x20(%rsp)\n"
    "  ud2\n"
    ".cfi_endproc\n");
#else
__asm__(
    ".text\n"
    ".globl _rex_redirect_trampoline\n"
    ".private_extern _rex_redirect_trampoline\n"
    ".p2align 2\n"
    "_rex_redirect_trampoline:\n"
    ".cfi_startproc\n"
    ".cfi_signal_frame\n"
    ".cfi_return_column 32\n"
    ".cfi_escape 0x0f, 0x03, 0x8f, 0x10, 0x06\n"
    ".cfi_escape 0x10, 0x1d, 0x02, 0x8f, 0x00\n"
    ".cfi_escape 0x10, 0x1e, 0x02, 0x8f, 0x08\n"
    ".cfi_escape 0x10, 0x20, 0x02, 0x8f, 0x18\n"
    "  ldr x16, [sp, #0x20]\n"
    "  blr x16\n"
    "  brk #1\n"
    ".cfi_endproc\n");
#endif
#endif

namespace rex::platform {

#if REX_PLATFORM_ANDROID
static void* g_android_libc;
// API 26+.
static int (*g_android_pthread_getname_np)(pthread_t pthread, char* buf, size_t n);

void AndroidInitialize() {
  if (rex::GetAndroidAPILevel() >= 26) {
    g_android_libc = dlopen("libc.so", RTLD_NOW);
    REX_ASSERT(g_android_libc != nullptr);
    if (g_android_libc) {
      g_android_pthread_getname_np = reinterpret_cast<decltype(g_android_pthread_getname_np)>(
          dlsym(g_android_libc, "pthread_getname_np"));
      REX_ASSERT(g_android_pthread_getname_np != nullptr);
    }
  }
}

void AndroidShutdown() {
  g_android_pthread_getname_np = nullptr;
  if (g_android_libc) {
    dlclose(g_android_libc);
    g_android_libc = nullptr;
  }
}
#endif

template <typename Rep, typename Period>
inline timespec DurationToTimeSpec(std::chrono::duration<Rep, Period> duration) {
  auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
  auto div = ldiv(nanoseconds.count(), 1000000000L);
  return timespec{div.quot, div.rem};
}

/** User defined signals that interrupt a thread, mapped onto SIGRTMIN through SIGRTMAX. */
enum class SignalType : u8 { kThreadSuspend, kThreadUserCallback, kCount };

int GetSystemSignal(SignalType num) {
#if defined(__APPLE__)
  // macOS has no realtime signals (SIGRTMIN..SIGRTMAX), so the 2 user defined signals are used.
  switch (num) {
    case SignalType::kThreadSuspend:
      return SIGUSR1;
    case SignalType::kThreadUserCallback:
      return SIGUSR2;
    default:
      REX_UNHANDLED_CASE(num);
      return SIGUSR1;
  }
#else
  auto result = SIGRTMIN + static_cast<int>(num);
  REX_ASSERT(result < SIGRTMAX);
  return result;
#endif
}

/** A mutex whose holder takes no thread suspend signal until it unlocks. */
class SuspendSafeMutex {
 public:
  void lock() {
    if (depth++ == 0) {
      sigset_t suspend;
      sigemptyset(&suspend);
      sigaddset(&suspend, GetSystemSignal(SignalType::kThreadSuspend));
      pthread_sigmask(SIG_BLOCK, &suspend, &saved_mask);
    }
    mutex_.lock();
  }

  void unlock() {
    mutex_.unlock();
    if (--depth == 0) {
      pthread_sigmask(SIG_SETMASK, &saved_mask, nullptr);
    }
  }

  /**
   * Waits on a condition with the lock held and the suspend signal blocked throughout.
   *
   * @tparam Predicate A callable returning true when the wait is over.
   * @param signal The condition notified under this mutex.
   * @param ready The predicate checked under this mutex.
   */
  template <typename Predicate>
  void Wait(std::condition_variable& signal, Predicate ready) {
    std::unique_lock inner(mutex_, std::adopt_lock);
    signal.wait(inner, ready);
    inner.release();
  }

 private:
  static inline thread_local u32 depth = 0;
  static inline thread_local sigset_t saved_mask;
  std::mutex mutex_;
};

SignalType GetSystemSignalType(int num) {
#if defined(__APPLE__)
  switch (num) {
    case SIGUSR1:
      return SignalType::kThreadSuspend;
    case SIGUSR2:
      return SignalType::kThreadUserCallback;
    default:
      REX_UNHANDLED_CASE(num);
      return SignalType::kThreadSuspend;
  }
#else
  return static_cast<SignalType>(num - SIGRTMIN);
#endif
}

std::array<std::atomic<bool>, static_cast<size_t>(SignalType::kCount)> g_signal_handler_installed =
    {};

static void SignalHandler(int signal, siginfo_t* info, void* context);

void InstallSignalHandler(SignalType type) {
  bool expected = false;
  if (!g_signal_handler_installed[static_cast<size_t>(type)].compare_exchange_strong(expected,
                                                                                     true)) {
    return;
  }
  struct sigaction action{};
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  action.sa_sigaction = SignalHandler;
  sigemptyset(&action.sa_mask);
  if (sigaction(GetSystemSignal(type), &action, nullptr) != 0) {
    g_signal_handler_installed[static_cast<size_t>(type)] = false;
  }
}

void WidenProcessAffinity() {}

u32 GetCallerSystemThreadID() {
#if defined(__APPLE__)
  return static_cast<u32>(pthread_mach_thread_np(pthread_self()));
#else
  return static_cast<u32>(syscall(SYS_gettid));
#endif
}

void MaybeYield() {
  sched_yield();
  __sync_synchronize();
}

void SyncMemory() {
  __sync_synchronize();
}

void Sleep(std::chrono::microseconds duration) {
  timespec rqtp = DurationToTimeSpec(duration);
  timespec rmtp = {};
  auto p_rqtp = &rqtp;
  auto p_rmtp = &rmtp;
  int ret = 0;
  do {
    ret = nanosleep(p_rqtp, p_rmtp);
    std::swap(p_rqtp, p_rmtp);
  } while (ret == -1 && errno == EINTR);
}

std::chrono::milliseconds GetCurrentThreadCPUTime() {
#if REX_PLATFORM_MAC
  thread_basic_info_data_t info{};
  mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
  if (thread_info(pthread_mach_thread_np(pthread_self()), THREAD_BASIC_INFO,
                  reinterpret_cast<thread_info_t>(&info), &count) != KERN_SUCCESS) {
    return {};
  }
  return std::chrono::seconds(info.user_time.seconds + info.system_time.seconds) +
         std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::microseconds(
             info.user_time.microseconds + info.system_time.microseconds));
#else
  timespec now{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return std::chrono::seconds(now.tv_sec) + std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::nanoseconds(now.tv_nsec));
#endif
}

using Deadline = std::chrono::steady_clock::time_point;

/**
 * Converts a timeout to a deadline.
 *
 * @param timeout The timeout, with max() meaning no deadline.
 * @return The deadline, or time_point::max() when there is none.
 */
Deadline MakeDeadline(std::chrono::milliseconds timeout) {
  if (timeout == std::chrono::milliseconds::max()) {
    return Deadline::max();
  }
  return std::chrono::steady_clock::now() + timeout;
}

/**
 * Reports whether a deadline has passed.
 *
 * @param deadline The time point to compare against now.
 * @return True when the deadline is finite and now is at or past it.
 */
bool DeadlinePassed(Deadline deadline) {
  return deadline != Deadline::max() && std::chrono::steady_clock::now() >= deadline;
}

/** Blocks 1 thread until an object it waits on changes or a user callback arrives. */
class Waiter {
 public:
  /** Marks a change and wakes the blocked thread. */
  void Wake() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++sequence_;
    changed_.notify_all();
  }

  /**
   * Returns the current change count.
   *
   * @return The change count.
   */
  u64 GetSequence() {
    std::lock_guard<std::mutex> lock(mutex_);
    return sequence_;
  }

  /**
   * Blocks until the change count differs from seen or the deadline passes.
   *
   * @param seen The count read before the state check.
   * @param deadline When to give up, or time_point::max() to wait forever.
   */
  void WaitChange(u64 seen, Deadline deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto changed = [&] { return sequence_ != seen; };
    if (deadline == Deadline::max()) {
      changed_.wait(lock, changed);
    } else {
      changed_.wait_until(lock, deadline, changed);
    }
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  u64 sequence_ = 0; /**< Protected by mutex_. */
};

Waiter& CurrentWaiter(bool alertable);
thread_local bool g_alertable_state = false;
bool DispatchCurrentThreadUserCallback();
SleepResult AlertableSleep(std::chrono::microseconds duration) {
  g_alertable_state = true;
  const auto deadline = std::chrono::steady_clock::now() + duration;
  Waiter& waiter = CurrentWaiter(true);
  while (true) {
    const u64 seen = waiter.GetSequence();
    if (DispatchCurrentThreadUserCallback()) {
      g_alertable_state = false;
      return SleepResult::kAlerted;
    }
    if (DeadlinePassed(deadline)) {
      g_alertable_state = false;
      return SleepResult::kSuccess;
    }
    waiter.WaitChange(seen, deadline);
  }
}

TLSHandle AllocateTLSHandle() {
  auto key = static_cast<pthread_key_t>(-1);
  auto res = pthread_key_create(&key, nullptr);
  REX_ASSERT(res == 0);
  REX_ASSERT(key != static_cast<pthread_key_t>(-1));
  return static_cast<TLSHandle>(key);
}

bool FreeTLSHandle(TLSHandle handle) {
  return pthread_key_delete(static_cast<pthread_key_t>(handle)) == 0;
}

uintptr_t GetTLSValue(TLSHandle handle) {
  return reinterpret_cast<uintptr_t>(pthread_getspecific(static_cast<pthread_key_t>(handle)));
}

bool SetTLSValue(TLSHandle handle, uintptr_t value) {
  // NOLINTNEXTLINE(performance-no-int-to-ptr): pthread passes the value as a void pointer
  return pthread_setspecific(static_cast<pthread_key_t>(handle), reinterpret_cast<void*>(value)) ==
         0;
}

#if defined(__APPLE__)
/**
 * Emulates an unnamed POSIX semaphore with a named one unlinked right after creation.
 *
 * macOS fails sem_init with ENOSYS. The descriptor stays valid until sem_close, and sem_wait
 * and sem_post stay async signal safe, which WaitSuspended() needs in a signal handler.
 *
 * @return The semaphore, or SEM_FAILED.
 */
static sem_t* CreateAnonymousSemaphore() {
  static std::atomic<u64> counter{0};
  char name[64];
  std::snprintf(name, sizeof(name), "/rexsusp_%d_%llu", static_cast<int>(getpid()),
                static_cast<unsigned long long>(counter.fetch_add(1, std::memory_order_relaxed)));
  sem_t* sem = sem_open(name, O_CREAT | O_EXCL, 0600, 0);
  if (sem != SEM_FAILED) {
    sem_unlink(name);
  }
  return sem;
}
#endif  // __APPLE__

bool CurrentThreadHasUserCallback();

class PosixConditionBase;

/** Registers a waiter on a set of objects for the lifetime of the scope. */
class WaitRegistration : public NonCopyable {
 public:
  WaitRegistration(Waiter& waiter, std::span<PosixConditionBase* const> conditions);
  ~WaitRegistration() { Release(); }

  /** Removes the waiter from every object. Safe to call again. */
  void Release();

 private:
  Waiter& waiter_;
  std::vector<PosixConditionBase*> conditions_;
};

class PosixConditionBase {
 public:
  PosixConditionBase() {
#if REX_PLATFORM_LINUX
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) == 0) {
      if (pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0) {
        auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
        pthread_mutex_destroy(native_mutex);
        pthread_mutex_init(native_mutex, &attr);
      }
      pthread_mutexattr_destroy(&attr);
    }
#endif
  }

  virtual ~PosixConditionBase() = default;
  virtual bool Signal() = 0;

  WaitResult Wait(std::chrono::milliseconds timeout) {
    bool executed;
    auto predicate = [this] { return this->GetSignaled(); };
    if (!Lock()) {
      return WaitResult::kFailed;
    }
    std::unique_lock<std::mutex> lock(mutex_, std::adopt_lock);
    if (predicate()) {
      executed = true;
    } else {
      if (timeout == std::chrono::milliseconds::max()) {
        cond_.wait(lock, predicate);
        executed = true;
      } else {
        executed = cond_.wait_for(lock, timeout, predicate);
      }
    }
    if (executed) {
      PostExecution();
      return TakeAbandoned() ? WaitResult::kAbandoned : WaitResult::kSuccess;
    } else {
      return WaitResult::kTimeout;
    }
  }

  /**
   * Blocks until the objects satisfy the wait, the deadline passes or, for an alertable wait, a
   * user callback arrives. The calling thread registers its waiter on every object for the
   * duration, so a signal on any of them wakes it.
   *
   * @param handles The objects to wait on.
   * @param wait_all True to wait for every object, false for any.
   * @param alertable True to let a queued user callback end the wait.
   * @param deadline When the wait times out, or time_point::max() to wait forever.
   * @return Why the wait ended and the index of the object for a success or an abandon.
   */
  static std::pair<WaitResult, size_t> WaitMultiple(std::span<PosixConditionBase* const> handles,
                                                    bool wait_all, bool alertable,
                                                    Deadline deadline) {
    REX_ASSERT(!handles.empty());
    Waiter& waiter = CurrentWaiter(alertable);
    std::vector<PosixConditionBase*> order(handles.begin(), handles.end());
    std::ranges::sort(order, std::less<>{});
    order.erase(std::ranges::unique(order).begin(), order.end());
    WaitRegistration registration(waiter, order);
    while (true) {
      const u64 seen = waiter.GetSequence();
      if (alertable && CurrentThreadHasUserCallback()) {
        registration.Release();
        DispatchCurrentThreadUserCallback();
        return std::make_pair(WaitResult::kUserCallback, size_t{0});
      }
      if (auto result = TryComplete(handles, order, wait_all)) {
        return *result;
      }
      if (DeadlinePassed(deadline)) {
        return std::make_pair(WaitResult::kTimeout, size_t{0});
      }
      waiter.WaitChange(seen, deadline);
    }
  }

  /**
   * Adds a waiter that every state change of this object wakes.
   *
   * @param waiter The waiter, which must stay alive until Unregister.
   */
  void Register(Waiter& waiter) {
    std::lock_guard<std::mutex> lock(mutex_);
    waiters_.push_back(&waiter);
  }

  /**
   * Removes a waiter added by Register.
   *
   * @param waiter The waiter to remove.
   */
  void Unregister(Waiter& waiter) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = std::ranges::find(waiters_, &waiter); it != waiters_.end()) {
      waiters_.erase(it);
    }
  }

  virtual void* GetNativeHandle() const {
    return const_cast<std::condition_variable&>(cond_).native_handle();
  }

 protected:
  inline virtual bool GetSignaled() const = 0;
  inline virtual void PostExecution() = 0;
  virtual bool TakeAbandoned() { return false; }

  /** Wakes everything blocked on this object. Callers hold mutex_. */
  void NotifyWaiters() {
    cond_.notify_all();
    for (Waiter* waiter : waiters_) {
      waiter->Wake();
    }
  }

  std::condition_variable cond_;
  std::mutex mutex_;
  std::vector<Waiter*> waiters_; /**< Protected by mutex_. */

 private:
  bool Lock() {
#if REX_PLATFORM_LINUX
    auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
    const int result = pthread_mutex_lock(native_mutex);
    if (result == EOWNERDEAD) {
      pthread_mutex_consistent(native_mutex);
      return true;
    }
    return result == 0;
#else
    mutex_.lock();
    return true;
#endif
  }

  void Unlock() { mutex_.unlock(); }

  static std::optional<std::pair<WaitResult, size_t>> TryComplete(
      std::span<PosixConditionBase* const> handles, std::span<PosixConditionBase* const> order,
      bool wait_all) {
    size_t locked = 0;
    const auto unlock = [&] {
      while (locked > 0) {
        order[--locked]->Unlock();
      }
    };
    for (PosixConditionBase* handle : order) {
      if (!handle->Lock()) {
        unlock();
        return std::make_pair(WaitResult::kFailed, size_t{0});
      }
      ++locked;
    }
    size_t first_signaled = std::numeric_limits<size_t>::max();
    bool condition_met = false;
    if (wait_all) {
      condition_met = std::ranges::all_of(handles, &PosixConditionBase::GetSignaled);
      first_signaled = 0;
    } else {
      for (size_t i = 0; i < handles.size(); ++i) {
        if (handles[i]->GetSignaled()) {
          first_signaled = i;
          condition_met = true;
          break;
        }
      }
    }
    if (!condition_met) {
      unlock();
      return std::nullopt;
    }
    size_t first_abandoned = std::numeric_limits<size_t>::max();
    if (wait_all) {
      for (size_t i = 0; i < handles.size(); ++i) {
        handles[i]->PostExecution();
        if (handles[i]->TakeAbandoned() && first_abandoned == std::numeric_limits<size_t>::max()) {
          first_abandoned = i;
        }
      }
    } else {
      handles[first_signaled]->PostExecution();
      if (handles[first_signaled]->TakeAbandoned()) {
        first_abandoned = first_signaled;
      }
    }
    unlock();
    if (first_abandoned != std::numeric_limits<size_t>::max()) {
      return std::make_pair(WaitResult::kAbandoned, first_abandoned);
    }
    return std::make_pair(WaitResult::kSuccess, first_signaled);
  }
};

/**
 * Handle for a wait and signal construct built on a mutex and condition variable, since POSIX
 * has no native handle for one.
 *
 * @tparam T The condition type.
 */
template <typename T>
class PosixCondition {};

template <>
class PosixCondition<Event> : public PosixConditionBase {
 public:
  PosixCondition(bool manual_reset, bool initial_state)
      : signal_(initial_state), manual_reset_(manual_reset) {}
  ~PosixCondition() override {
    if (poll_read_ != -1) {
      close(poll_read_);
    }
    if (poll_write_ != -1 && poll_write_ != poll_read_) {
      close(poll_write_);
    }
  }

  bool Signal() override {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    Raise();
    NotifyWaiters();
    return true;
  }

  void Reset() {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    Lower();
  }

  /**
   * Returns a descriptor that polls readable while the event is set, creating it on first use.
   *
   * @return The descriptor, or -1 when it cannot be created.
   */
  int GetPollFD() {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    if (poll_read_ != -1) {
      return poll_read_;
    }
#if defined(__APPLE__)
    int ends[2];
    if (pipe(ends) != 0) {
      return -1;
    }
    for (const int end : ends) {
      const int flags = fcntl(end, F_GETFL);
      if (flags == -1 || fcntl(end, F_SETFL, flags | O_NONBLOCK) == -1 ||
          fcntl(end, F_SETFD, FD_CLOEXEC) == -1) {
        close(ends[0]);
        close(ends[1]);
        return -1;
      }
    }
    poll_read_ = ends[0];
    poll_write_ = ends[1];
#else
    poll_read_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    poll_write_ = poll_read_;
    if (poll_read_ == -1) {
      return -1;
    }
#endif
    if (signal_) {
      Post();
    }
    return poll_read_;
  }

 private:
  bool GetSignaled() const override { return signal_; }
  void PostExecution() override {
    if (!manual_reset_) {
      Lower();
    }
  }
  void Raise() {
    if (!signal_) {
      signal_ = true;
      Post();
    }
  }
  void Lower() {
    if (signal_) {
      signal_ = false;
      if (poll_read_ != -1) {
        u64 count;
        [[maybe_unused]] const ssize_t drained = read(poll_read_, &count, sizeof(count));
      }
    }
  }
  void Post() const {
    if (poll_write_ != -1) {
      const u64 one = 1;
      [[maybe_unused]] const ssize_t posted = write(poll_write_, &one, sizeof(one));
    }
  }
  bool signal_;
  const bool manual_reset_;
  int poll_read_ = -1;
  int poll_write_ = -1;
};

template <>
class PosixCondition<Semaphore> : public PosixConditionBase {
 public:
  PosixCondition(int initial_count, int maximum_count)
      : count_(initial_count), maximum_count_(maximum_count) {}

  bool Signal() override { return Release(1, nullptr); }

  bool Release(int release_count, int* out_previous_count) {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    if (release_count > maximum_count_ - count_) {
      return false;
    }
    if (out_previous_count) {
      *out_previous_count = count_;
    }
    count_ += release_count;
    NotifyWaiters();
    return true;
  }

 private:
  bool GetSignaled() const override { return count_ > 0; }
  void PostExecution() override {
    count_--;
    NotifyWaiters();
  }
  int count_;
  const int maximum_count_;
};

namespace {
void ArmMutantAbandonOnExit();
}  // namespace

template <>
class PosixCondition<Mutant> : public PosixConditionBase {
 public:
  explicit PosixCondition(bool initial_owner) {
    if (initial_owner) {
      count_ = 1;
      owner_ = pthread_self();
      ArmMutantAbandonOnExit();
    }
    auto& registry = Registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.mutants.push_back(this);
  }

  ~PosixCondition() override {
    auto& registry = Registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    std::erase(registry.mutants, this);
  }

  bool Signal() override { return Release(); }

  bool Release() {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    if (count_ == 0 || !pthread_equal(owner_, pthread_self())) {
      return false;
    }
    --count_;
    if (count_ == 0) {
      NotifyWaiters();
    }
    return true;
  }

  static void AbandonOwnedBy(pthread_t thread) {
    auto& registry = Registry();
    std::lock_guard<std::mutex> registry_lock(registry.mutex);
    for (PosixCondition* mutant : registry.mutants) {
      auto lock = std::unique_lock<std::mutex>(mutant->mutex_);
      if (mutant->count_ == 0 || !pthread_equal(mutant->owner_, thread)) {
        continue;
      }
      mutant->count_ = 0;
      mutant->abandoned_ = true;
      mutant->NotifyWaiters();
    }
  }

  void* GetNativeHandle() const override { return const_cast<std::mutex&>(mutex_).native_handle(); }

 private:
  struct MutantRegistry {
    std::mutex mutex;
    std::vector<PosixCondition*> mutants;
  };

  static MutantRegistry& Registry() {
    static MutantRegistry registry;
    return registry;
  }

  bool GetSignaled() const override { return count_ == 0 || pthread_equal(owner_, pthread_self()); }
  void PostExecution() override {
    count_++;
    owner_ = pthread_self();
    ArmMutantAbandonOnExit();
  }
  bool TakeAbandoned() override { return std::exchange(abandoned_, false); }
  u32 count_ = 0;
  pthread_t owner_{};
  bool abandoned_ = false;
};

namespace {

struct MutantExitGuard {
  bool armed = false;
  ~MutantExitGuard() {
    if (armed) {
      PosixCondition<Mutant>::AbandonOwnedBy(pthread_self());
    }
  }
};

thread_local MutantExitGuard t_mutant_exit_guard;

void ArmMutantAbandonOnExit() {
  t_mutant_exit_guard.armed = true;
}

}  // namespace

template <>
class PosixCondition<Timer> : public PosixConditionBase {
 public:
  explicit PosixCondition(bool manual_reset)
      : callback_(nullptr), signal_(false), manual_reset_(manual_reset) {}

  virtual ~PosixCondition() { Cancel(); }

  bool Signal() override {
    std::lock_guard<std::mutex> lock(mutex_);
    signal_ = true;
    NotifyWaiters();
    return true;
  }

  void SetOnce(std::chrono::steady_clock::time_point due_time, std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard<std::mutex> lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ = QueueTimerOnce(&CompletionRoutine, this, due_time);
  }

  void SetRepeating(std::chrono::steady_clock::time_point due_time,
                    std::chrono::milliseconds period, std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard<std::mutex> lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ = QueueTimerRecurring(&CompletionRoutine, this, due_time, period);
  }

  void Cancel() {
    if (auto wait_item = wait_item_.lock()) {
      wait_item->Disarm();
    }
  }

  void Reset() {
    Cancel();
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = nullptr;
    signal_ = false;
  }

  void* GetNativeHandle() const override {
    REX_ASSERT_ALWAYS();
    return nullptr;
  }

 private:
  static void CompletionRoutine(void* userdata) {
    REX_ASSERT(userdata != nullptr);
    auto timer = reinterpret_cast<PosixCondition<Timer>*>(userdata);
    timer->Signal();
    std::function<void()> callback;
    {
      std::lock_guard<std::mutex> lock(timer->mutex_);
      callback = timer->callback_;
    }
    if (callback) {
      callback();
    }
  }

  bool GetSignaled() const override { return signal_; }
  void PostExecution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  std::weak_ptr<TimerQueueWaitItem> wait_item_;
  std::function<void()> callback_;
  bool signal_; /**< Protected by mutex_. */
  const bool manual_reset_;
};

struct ThreadStartArgs {
  rex::MoveOnlyFunction<void()> start_routine;
  bool create_suspended;
  Thread* thread_obj;
};

template <>
class PosixCondition<Thread> : public PosixConditionBase {
  enum class State : u8 {
    kUninitialized,
    kRunning,
    kSuspended,
    kFinished,
  };

 public:
  PosixCondition()
      : thread_(0),
        signaled_(false),
        exit_code_(0),
        state_(State::kUninitialized),
        suspend_count_(0) {
#if defined(__APPLE__)
    suspend_sem_ = CreateAnonymousSemaphore();
#else
    sem_init(&suspend_sem_, 0, 0);
#endif
#if REX_PLATFORM_ANDROID
    android_pre_api_26_name_[0] = '\0';
#endif
  }
  bool Initialize(Thread::CreationParameters params, std::unique_ptr<ThreadStartArgs> start_data) {
    start_data->create_suspended = params.create_suspended;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
      return false;
    if (pthread_attr_setstacksize(&attr, params.stack_size) != 0) {
      pthread_attr_destroy(&attr);
      return false;
    }
    if (params.initial_priority != 0) {
      sched_param sched{};
      sched.sched_priority = params.initial_priority + 1;
      if (pthread_attr_setschedpolicy(&attr, SCHED_FIFO) != 0) {
        pthread_attr_destroy(&attr);
        return false;
      }
      if (pthread_attr_setschedparam(&attr, &sched) != 0) {
        pthread_attr_destroy(&attr);
        return false;
      }
    }
    ThreadStartArgs* const start = start_data.release();
    if (pthread_create(&thread_, &attr, ThreadStartRoutine, start) != 0) {
      start_data.reset(start);
      pthread_attr_destroy(&attr);
      return false;
    }
    owns_thread_ = true;
    pthread_attr_destroy(&attr);
    return true;
  }

  /**
   * Wraps a running thread.
   *
   * @param thread The running pthread to wrap.
   */
  explicit PosixCondition(pthread_t thread)
      : thread_(thread),
        signaled_(false),
        exit_code_(0),
        state_(State::kRunning),
        suspend_count_(0) {
#if defined(__APPLE__)
    suspend_sem_ = CreateAnonymousSemaphore();
#else
    sem_init(&suspend_sem_, 0, 0);
#endif
#if REX_PLATFORM_ANDROID
    android_pre_api_26_name_[0] = '\0';
#endif
  }

  virtual ~PosixCondition() {
#if defined(__APPLE__)
    if (wake_read_ != -1) {
      close(wake_read_);
      close(wake_write_.load());
    }
#endif
    if (!owns_thread_ || std::exchange(joined_, true)) {
      return;
    }
    if (pthread_equal(thread_, pthread_self())) {
      pthread_detach(thread_);
    } else {
      pthread_join(thread_, nullptr);
    }
  }

  bool Signal() override { return true; }

  std::string GetName() const {
    WaitStarted();
    auto result = std::array<char, 17>{'\0'};
    std::unique_lock lock(state_mutex_);
    if (state_ != State::kUninitialized && state_ != State::kFinished) {
#if REX_PLATFORM_ANDROID
      // Below API 26 pthread_getname_np is absent, so the name is stored in this object.
      if (g_android_pthread_getname_np) {
        if (g_android_pthread_getname_np(thread_, result.data(), result.size() - 1) != 0) {
          REX_ASSERT_ALWAYS();
        }
      } else {
        std::lock_guard<std::mutex> lock(android_pre_api_26_name_mutex_);
        rex::copy_truncating(result.data(), android_pre_api_26_name_, result.size());
      }
#else
      if (pthread_getname_np(thread_, result.data(), result.size() - 1) != 0) {
        REX_ASSERT_ALWAYS();
      }
#endif
    }
    return std::string(result.data());
  }

  void SetName(const std::string& name) {
    WaitStarted();
    std::unique_lock lock(state_mutex_);
    if (state_ != State::kUninitialized && state_ != State::kFinished) {
#if defined(__APPLE__)
      // macOS pthread_setname_np() only names the calling thread.
      if (pthread_equal(thread_, pthread_self())) {
        pthread_setname_np(std::string(name).c_str());
      }
#else
      pthread_setname_np(thread_, std::string(name).c_str());
#endif
#if REX_PLATFORM_ANDROID
      SetAndroidPreAPI26Name(name);
#endif
    }
  }

#if REX_PLATFORM_ANDROID
  void SetAndroidPreAPI26Name(const std::string_view name) {
    if (g_android_pthread_getname_np) {
      return;
    }
    std::lock_guard<std::mutex> lock(android_pre_api_26_name_mutex_);
    rex::copy_truncating(android_pre_api_26_name_, name, rex::CountOf(android_pre_api_26_name_));
  }
#endif

#if defined(__APPLE__)
  u32 GetSystemId() const { return static_cast<u32>(pthread_mach_thread_np(thread_)); }
#else
  u32 GetSystemId() const { return static_cast<u32>(thread_); }
#endif

  u64 GetAffinityMask() const {
    WaitStarted();
#if defined(__APPLE__)
    // macOS has no pthread affinity masks, so every logical processor is reported.
    auto cpu_count = std::min<u32>(GetLogicalProcessorCount(), 64);
    if (cpu_count == 64) {
      return std::numeric_limits<u64>::max();
    }
    return (static_cast<u64>(1) << cpu_count) - 1;
#else
    cpu_set_t cpu_set;
#if REX_PLATFORM_ANDROID
    if (sched_getaffinity(pthread_gettid_np(thread_), sizeof(cpu_set_t), &cpu_set) != 0) {
      REX_ASSERT_ALWAYS();
    }
#else
    if (pthread_getaffinity_np(thread_, sizeof(cpu_set_t), &cpu_set) != 0) {
      REX_ASSERT_ALWAYS();
    }
#endif
    u64 result = 0;
    auto cpu_count = std::min(CPU_SETSIZE, 64);
    for (int i = 0; i < cpu_count; i++) {
      auto set = CPU_ISSET(i, &cpu_set);
      result |= set << i;
    }
    return result;
#endif
  }

  void SetAffinityMask(u64 mask) const {
    WaitStarted();
#if defined(__APPLE__)
    // macOS has no pthread affinity masks, so this does nothing.
    (void)mask;
#else
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    for (auto i = 0u; i < 64; i++) {
      if (mask & (1ULL << i)) {
        CPU_SET(i, &cpu_set);
      }
    }
#if REX_PLATFORM_ANDROID
    if (sched_setaffinity(pthread_gettid_np(thread_), sizeof(cpu_set_t), &cpu_set) != 0) {
      REX_ASSERT_ALWAYS();
    }
#else
    if (pthread_setaffinity_np(thread_, sizeof(cpu_set_t), &cpu_set) != 0) {
      REX_ASSERT_ALWAYS();
    }
#endif
#endif
  }

  int GetPriority() const {
    WaitStarted();
    int policy;
    sched_param param{};
    int ret = pthread_getschedparam(thread_, &policy, &param);
    if (ret != 0) {
      return -1;
    }

    return param.sched_priority;
  }

  void SetPriority(int new_priority) const {
    WaitStarted();
    sched_param param{};
    param.sched_priority = new_priority;
    int result = pthread_setschedparam(thread_, SCHED_FIFO, &param);
    if (result != 0) {
      switch (result) {
        case EPERM:
          REXLOG_WARN(system, "SetPriority: permission denied");
          break;
        case EINVAL:
          REX_ASSERT_ALWAYS();
          break;
        default:
          REXLOG_WARN(system, "SetPriority: pthread_setschedparam failed ({})", result);
          break;
      }
    }
  }

  void QueueUserCallback(rex::MoveOnlyFunction<void()> callback) {
    WaitStarted();
    std::unique_lock lock(state_mutex_);
    if (state_ == State::kFinished) {
      return;
    }
    {
      std::unique_lock<std::mutex> callback_lock(callback_mutex_);
      user_callbacks_.push_back(std::move(callback));
      has_pending_user_callbacks_.store(true);
    }
    waiter_.Wake();
#if defined(__APPLE__)
    SignalWake();
#endif

    // Alertable waits drain the queue on the current thread, so no self signal is sent.
    if (pthread_equal(thread_, pthread_self())) {
      lock.unlock();
      if (g_alertable_state) {
        DispatchQueuedUserCallbacks();
      }
      return;
    }

#if defined(__APPLE__)
    // macOS has no sigqueue, so pthread_kill delivers the signal without the sigval payload.
    int result = pthread_kill(thread_, GetSystemSignal(SignalType::kThreadUserCallback));
#else
    sigval value{};
    value.sival_ptr = this;
#if REX_PLATFORM_ANDROID
    int result = sigqueue(pthread_gettid_np(thread_),
                          GetSystemSignal(SignalType::kThreadUserCallback), value);
#else
    int result = pthread_sigqueue(thread_, GetSystemSignal(SignalType::kThreadUserCallback), value);
#endif
#endif
    lock.unlock();
    if (result != 0) {
      REXLOG_WARN(system, "QueueUserCallback: signal delivery failed ({})", result);
    }
  }

  Waiter& GetWaiter() { return waiter_; }

  bool HasPendingUserCallbacks() const { return has_pending_user_callbacks_.load(); }

#if defined(__APPLE__)
  /**
   * Returns a descriptor that polls readable after a user callback is queued, creating it on
   * first use. Only the owning thread calls this.
   *
   * @return The descriptor, or -1 when it cannot be created.
   */
  int WakeReadFD() {
    if (wake_read_ != -1) {
      return wake_read_;
    }
    int ends[2];
    if (pipe(ends) != 0) {
      return -1;
    }
    for (const int end : ends) {
      const int flags = fcntl(end, F_GETFL);
      if (flags == -1 || fcntl(end, F_SETFL, flags | O_NONBLOCK) == -1 ||
          fcntl(end, F_SETFD, FD_CLOEXEC) == -1) {
        close(ends[0]);
        close(ends[1]);
        return -1;
      }
    }
    wake_read_ = ends[0];
    wake_write_.store(ends[1]);
    return wake_read_;
  }

  /** Empties the wake descriptor. Only the owning thread calls this. */
  void DrainWake() {
    if (wake_read_ == -1) {
      return;
    }
    char sink[64];
    while (read(wake_read_, sink, sizeof(sink)) > 0) {}
  }
#endif

  bool DispatchQueuedUserCallbacks() {
    if (!has_pending_user_callbacks_.load(std::memory_order_acquire)) {
      return false;
    }

    rex::MoveOnlyFunction<void()> callback;
    {
      std::unique_lock<std::mutex> lock(callback_mutex_);
      if (user_callbacks_.empty()) {
        has_pending_user_callbacks_.store(false, std::memory_order_release);
        return false;
      }
      callback = std::move(user_callbacks_.front());
      user_callbacks_.pop_front();
      has_pending_user_callbacks_.store(!user_callbacks_.empty(), std::memory_order_release);
    }
    if (callback) {
      callback();
    }
    return true;
  }

  bool Resume(u32* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    std::unique_lock lock(state_mutex_);
    if (suspend_count_ == 0) {
      return false;
    }
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = suspend_count_;
    }
    --suspend_count_;
    if (suspend_count_ == 0 && (state_ == State::kSuspended || state_ == State::kFinished)) {
      if (state_ == State::kSuspended) {
        state_ = State::kRunning;
      }
      // Async signal safe wakeup for WaitSuspended() from signal handler path.
      sem_post(GetSuspendSemPtr());
    }
    state_signal_.notify_all();
    return true;
  }

  bool Suspend(u32* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    bool is_current_thread = pthread_self() == thread_;
    std::unique_lock lock(state_mutex_);
    if (state_ == State::kFinished) {
      return false;
    }
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = suspend_count_;
    }
    state_ = State::kSuspended;
    const bool already_suspended = suspend_count_++ > 0;
    if (is_current_thread) {
      lock.unlock();
      WaitSuspended();
      return true;
    }
    if (already_suspended) {
      return true;
    }
    park_requests_.fetch_add(1, std::memory_order_acq_rel);
    return pthread_kill(thread_, GetSystemSignal(SignalType::kThreadSuspend)) == 0;
  }

  bool SuspendIf(Thread::RedirectFilter filter, void* context) {
    WaitStarted();
    if (pthread_equal(thread_, pthread_self())) {
      return false;
    }
    std::lock_guard redirect_lock(redirect_mutex_);
    {
      std::unique_lock lock(state_mutex_);
      if (state_ == State::kFinished) {
        return false;
      }
      if (suspend_count_ > 0) {
        ++suspend_count_;
        return true;
      }
      suspend_count_ = 1;
      state_ = State::kSuspended;
      suspend_filter_ = filter;
      suspend_filter_context_ = context;
      suspend_result_.store(kRedirectWaiting, std::memory_order_relaxed);
      suspend_pending_.store(true, std::memory_order_release);
      if (pthread_kill(thread_, GetSystemSignal(SignalType::kThreadSuspend)) != 0) {
        suspend_pending_.store(false, std::memory_order_relaxed);
        suspend_count_ = 0;
        state_ = State::kRunning;
        return false;
      }
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (suspend_result_.load(std::memory_order_acquire) == kRedirectWaiting) {
      if (std::chrono::steady_clock::now() >= deadline &&
          suspend_pending_.exchange(false, std::memory_order_acq_rel)) {
        break;
      }
      sched_yield();
    }
    if (suspend_result_.load(std::memory_order_acquire) == kRedirectTaken) {
      return true;
    }
    std::unique_lock lock(state_mutex_);
    suspend_count_ = 0;
    state_ = State::kRunning;
    return false;
  }

  void HandleSuspend(void* signal_context) {
    if (suspend_pending_.exchange(false, std::memory_order_acq_rel)) {
      if (!suspend_filter_(detail::SignalPC(signal_context), suspend_filter_context_)) {
        suspend_result_.store(kRedirectRefused, std::memory_order_release);
        return;
      }
      suspend_result_.store(kRedirectTaken, std::memory_order_release);
      WaitSuspended();
      return;
    }
    if (park_requests_.load(std::memory_order_acquire) > 0) {
      park_requests_.fetch_sub(1, std::memory_order_acq_rel);
      WaitSuspended();
    }
  }

  bool Redirect(Thread::RedirectFilter filter, void* context, void (*target)()) {
    WaitStarted();
    if (pthread_equal(thread_, pthread_self())) {
      return false;
    }
    std::lock_guard lock(redirect_mutex_);
    InstallSignalHandler(SignalType::kThreadUserCallback);
    redirect_filter_ = filter;
    redirect_context_ = context;
    redirect_target_ = target;
    redirect_result_.store(kRedirectWaiting, std::memory_order_relaxed);
    redirect_pending_.store(true, std::memory_order_release);
    {
      std::lock_guard state_lock(state_mutex_);
      if (state_ == State::kFinished ||
          pthread_kill(thread_, GetSystemSignal(SignalType::kThreadUserCallback)) != 0) {
        redirect_pending_.store(false, std::memory_order_relaxed);
        return false;
      }
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (redirect_result_.load(std::memory_order_acquire) == kRedirectWaiting) {
      if (std::chrono::steady_clock::now() >= deadline &&
          redirect_pending_.exchange(false, std::memory_order_acq_rel)) {
        return false;
      }
      sched_yield();
    }
    return redirect_result_.load(std::memory_order_acquire) == kRedirectTaken;
  }

  void HandleRedirect([[maybe_unused]] int signal, void* signal_context) {
    if (!redirect_pending_.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    const auto target = redirect_target_;
    if (!redirect_filter_(detail::SignalPC(signal_context), redirect_context_)) {
      redirect_result_.store(kRedirectRefused, std::memory_order_release);
      return;
    }
    redirect_result_.store(kRedirectTaken, std::memory_order_release);
#if REX_PLATFORM_MAC
    detail::CallOnSignalReturn(signal_context, target);
#else
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, signal);
    pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
    target();
#endif
  }

  [[noreturn]] void ExitCurrent(int exit_code) {
    {
      std::unique_lock lock(state_mutex_);
      state_ = State::kFinished;
    }
    PosixCondition<Mutant>::AbandonOwnedBy(thread_);
    SignalExit(exit_code);
    // NOLINTNEXTLINE(performance-no-int-to-ptr): pthread passes the value as a void pointer
    pthread_exit(reinterpret_cast<void*>(exit_code));
  }

  void WaitStarted() const {
    std::unique_lock lock(state_mutex_);
    state_mutex_.Wait(state_signal_, [this] { return state_ != State::kUninitialized; });
  }

  /** Uses sem_wait, since it can run in signal handler context. */
  void WaitSuspended() {
    int ret;
    do {
      ret = sem_wait(GetSuspendSemPtr());
    } while (ret == -1 && errno == EINTR);
  }

  // NOLINTNEXTLINE(performance-no-int-to-ptr): the cross platform native handle is a pointer
  void* GetNativeHandle() const override { return reinterpret_cast<void*>(thread_); }

 private:
  static void* ThreadStartRoutine(void* parameter);
  void SignalExit(int exit_code) {
    std::lock_guard<std::mutex> lock(mutex_);
    exit_code_ = exit_code;
    signaled_ = true;
    NotifyWaiters();
  }
  bool GetSignaled() const override { return signaled_; }
  void PostExecution() override {
    if (thread_ && !std::exchange(joined_, true)) {
      pthread_join(thread_, nullptr);
    }
#if defined(__APPLE__)
    if (suspend_sem_ != SEM_FAILED) {
      sem_close(suspend_sem_);
      suspend_sem_ = SEM_FAILED;
    }
#else
    sem_destroy(&suspend_sem_);
#endif
  }
  static constexpr int kRedirectWaiting = 0;
  static constexpr int kRedirectRefused = 1;
  static constexpr int kRedirectTaken = 2;

  pthread_t thread_;
  std::mutex redirect_mutex_;
  std::atomic<bool> redirect_pending_{false};
  std::atomic<int> redirect_result_{kRedirectWaiting};
  Thread::RedirectFilter redirect_filter_ = nullptr;
  void* redirect_context_ = nullptr;
  void (*redirect_target_)() = nullptr;
  std::atomic<bool> suspend_pending_{false};
  std::atomic<int> suspend_result_{kRedirectWaiting};
  std::atomic<u32> park_requests_{0};
  Thread::RedirectFilter suspend_filter_ = nullptr;
  void* suspend_filter_context_ = nullptr;
  bool signaled_;
  bool joined_ = false;
  bool owns_thread_ = false;
  int exit_code_;
  State state_;       /**< Protected by state_mutex_. */
  u32 suspend_count_; /**< Protected by state_mutex_. */
#if defined(__APPLE__)
  sem_t* suspend_sem_ = SEM_FAILED;
  sem_t* GetSuspendSemPtr() { return suspend_sem_; }
#else
  sem_t suspend_sem_;
  sem_t* GetSuspendSemPtr() { return &suspend_sem_; }
#endif
  mutable SuspendSafeMutex state_mutex_;
  mutable std::mutex callback_mutex_;
  mutable std::condition_variable state_signal_;
  std::deque<rex::MoveOnlyFunction<void()>> user_callbacks_;
  std::atomic<bool> has_pending_user_callbacks_{false};
  Waiter waiter_;
#if defined(__APPLE__)
  void SignalWake() {
    const int fd = wake_write_.load();
    if (fd != -1) {
      const char one = 1;
      [[maybe_unused]] const ssize_t posted = write(fd, &one, sizeof(one));
    }
  }
  int wake_read_ = -1;
  std::atomic<int> wake_write_{-1};
#endif
#if REX_PLATFORM_ANDROID
  // Below API 26 the name is stored here since pthread_getname_np is absent.
  mutable std::mutex android_pre_api_26_name_mutex_;
  char android_pre_api_26_name_[16];
#endif
};

class PosixWaitHandle {
 public:
  virtual ~PosixWaitHandle();
  virtual PosixConditionBase& GetCondition() = 0;
};

PosixWaitHandle::~PosixWaitHandle() = default;

thread_local PosixCondition<Thread>* g_current_thread_condition = nullptr;

bool DispatchCurrentThreadUserCallback() {
  if (!g_current_thread_condition) {
    Thread::GetCurrentThread();
  }
  return g_current_thread_condition && g_current_thread_condition->DispatchQueuedUserCallbacks();
}

namespace {

class ScopedAlertableState {
 public:
  explicit ScopedAlertableState(bool alertable) : alertable_(alertable) {
    if (alertable_) {
      g_alertable_state = true;
    }
  }
  ~ScopedAlertableState() {
    if (alertable_) {
      g_alertable_state = false;
    }
  }

 private:
  bool alertable_;
};

PosixCondition<Thread>* CurrentThreadCondition() {
  if (!g_current_thread_condition) {
    Thread::GetCurrentThread();
  }
  return g_current_thread_condition;
}

}  // namespace

bool CurrentThreadHasUserCallback() {
  auto* thread = CurrentThreadCondition();
  return thread && thread->HasPendingUserCallbacks();
}

Waiter& CurrentWaiter(bool alertable) {
  auto* thread = alertable ? CurrentThreadCondition() : g_current_thread_condition;
  if (thread) {
    return thread->GetWaiter();
  }
  static thread_local Waiter plain_thread_waiter;
  return plain_thread_waiter;
}

WaitRegistration::WaitRegistration(Waiter& waiter, std::span<PosixConditionBase* const> conditions)
    : waiter_(waiter), conditions_(conditions.begin(), conditions.end()) {
  for (PosixConditionBase* condition : conditions_) {
    condition->Register(waiter_);
  }
}

void WaitRegistration::Release() {
  for (PosixConditionBase* condition : conditions_) {
    condition->Unregister(waiter_);
  }
  conditions_.clear();
}

/**
 * Wraps a condition object as the handle, since POSIX has no native handle for higher level
 * concurrency constructs such as semaphores.
 *
 * @tparam T The condition type.
 */
template <typename T>
class PosixConditionHandle : public T, public PosixWaitHandle {
 public:
  PosixConditionHandle() = default;
  explicit PosixConditionHandle(bool);
  explicit PosixConditionHandle(pthread_t thread);
  PosixConditionHandle(bool manual_reset, bool initial_state);
  PosixConditionHandle(int initial_count, int maximum_count);
  ~PosixConditionHandle() override = default;

  PosixCondition<T>& GetCondition() override { return handle_; }
  void* GetNativeHandle() const override { return handle_.GetNativeHandle(); }

 protected:
  PosixCondition<T> handle_;
  friend PosixCondition<T>;
};

template <>
PosixConditionHandle<Semaphore>::PosixConditionHandle(int initial_count, int maximum_count)
    : handle_(initial_count, maximum_count) {}

template <>
PosixConditionHandle<Mutant>::PosixConditionHandle(bool initial_owner) : handle_(initial_owner) {}

template <>
PosixConditionHandle<Timer>::PosixConditionHandle(bool manual_reset) : handle_(manual_reset) {}

template <>
PosixConditionHandle<Event>::PosixConditionHandle(bool manual_reset, bool initial_state)
    : handle_(manual_reset, initial_state) {}

template <>
PosixConditionHandle<Thread>::PosixConditionHandle(pthread_t thread) : handle_(thread) {}

WaitResult Wait(WaitHandle* wait_handle, bool is_alertable, std::chrono::milliseconds timeout) {
  auto posix_wait_handle = dynamic_cast<PosixWaitHandle*>(wait_handle);
  if (posix_wait_handle == nullptr) {
    return WaitResult::kFailed;
  }
  if (!is_alertable) {
    return posix_wait_handle->GetCondition().Wait(timeout);
  }
  ScopedAlertableState alertable_state_guard(true);
  PosixConditionBase* condition = &posix_wait_handle->GetCondition();
  return PosixConditionBase::WaitMultiple(std::span(&condition, 1), true, true,
                                          MakeDeadline(timeout))
      .first;
}

namespace {

int EventPollFD(WaitHandle* event) {
  auto* condition = dynamic_cast<PosixConditionHandle<Event>*>(event);
  return condition ? condition->GetCondition().GetPollFD() : -1;
}

}  // namespace

int SocketPrepare(SocketHandle handle, WaitHandle* event) {
  const int flags = fcntl(static_cast<int>(handle), F_GETFL);
  if (flags == -1) {
    return -1;
  }
#if defined(SO_NOSIGPIPE)
  const int enable = 1;
  if (setsockopt(static_cast<int>(handle), SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable)) !=
      0) {
    return -1;
  }
#endif
  if (fcntl(static_cast<int>(handle), F_SETFL, flags | O_NONBLOCK) == -1) {
    return -1;
  }
  return EventPollFD(event) == -1 ? -1 : 0;
}

WaitResult WaitSockets(std::span<const SocketWait> sockets, bool alertable,
                       std::chrono::milliseconds timeout) {
  std::vector<pollfd> fds;
  fds.reserve(sockets.size());
  for (const SocketWait& socket : sockets) {
    short events = 0;
    if ((socket.ready & SocketReady::kRead) != SocketReady::kNone) {
      events |= POLLIN;
    }
    if ((socket.ready & SocketReady::kWrite) != SocketReady::kNone) {
      events |= POLLOUT;
    }
    if ((socket.ready & SocketReady::kExcept) != SocketReady::kNone) {
      events |= POLLPRI;
    }
    fds.push_back({static_cast<int>(socket.handle), events, 0});
  }
  for (const SocketWait& socket : sockets) {
    const int event_fd = EventPollFD(socket.event);
    if (event_fd != -1 &&
        std::ranges::none_of(fds, [event_fd](const pollfd& fd) { return fd.fd == event_fd; })) {
      fds.push_back({event_fd, POLLIN, 0});
    }
  }
  const auto deadline = MakeDeadline(timeout);
  ScopedAlertableState alertable_state_guard(alertable);
#if defined(__APPLE__)
  PosixCondition<Thread>* thread = alertable ? CurrentThreadCondition() : nullptr;
  const size_t socket_fds = fds.size();
  if (thread && thread->WakeReadFD() != -1) {
    fds.push_back({thread->WakeReadFD(), POLLIN, 0});
  }
  while (true) {
    if (thread) {
      thread->DrainWake();
    }
    if (alertable && DispatchCurrentThreadUserCallback()) {
      return WaitResult::kUserCallback;
    }
    int poll_timeout = -1;
    if (deadline != Deadline::max()) {
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
          std::max(deadline - std::chrono::steady_clock::now(),
                   std::chrono::steady_clock::duration::zero()));
      poll_timeout = static_cast<int>(remaining.count());
    }
    const int ready = poll(fds.data(), fds.size(), poll_timeout);
    if (ready > 0 && std::any_of(fds.begin(), fds.begin() + socket_fds,
                                 [](const pollfd& fd) { return fd.revents != 0; })) {
      return WaitResult::kSuccess;
    }
    if (ready < 0 && errno != EINTR) {
      return WaitResult::kFailed;
    }
    if (DeadlinePassed(deadline)) {
      return WaitResult::kTimeout;
    }
  }
#else
  sigset_t callback;
  sigemptyset(&callback);
  sigaddset(&callback, GetSystemSignal(SignalType::kThreadUserCallback));
  sigset_t saved;
  pthread_sigmask(SIG_BLOCK, &callback, &saved);
  sigset_t during = saved;
  if (alertable) {
    sigdelset(&during, GetSystemSignal(SignalType::kThreadUserCallback));
  }
  WaitResult result = WaitResult::kFailed;
  while (true) {
    if (alertable && DispatchCurrentThreadUserCallback()) {
      result = WaitResult::kUserCallback;
      break;
    }
    timespec left{};
    timespec* left_ptr = nullptr;
    if (deadline != std::chrono::steady_clock::time_point::max()) {
      left = DurationToTimeSpec(std::max(deadline - std::chrono::steady_clock::now(),
                                         std::chrono::steady_clock::duration::zero()));
      left_ptr = &left;
    }
    const int ready = ppoll(fds.data(), fds.size(), left_ptr, &during);
    if (ready > 0) {
      result = WaitResult::kSuccess;
      break;
    }
    if (ready == 0) {
      result = WaitResult::kTimeout;
      break;
    }
    if (errno != EINTR) {
      break;
    }
  }
  pthread_sigmask(SIG_SETMASK, &saved, nullptr);
  return result;
#endif
}

WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal, WaitHandle* wait_handle_to_wait_on,
                         bool is_alertable, std::chrono::milliseconds timeout) {
  auto posix_wait_handle_to_signal = dynamic_cast<PosixWaitHandle*>(wait_handle_to_signal);
  auto posix_wait_handle_to_wait_on = dynamic_cast<PosixWaitHandle*>(wait_handle_to_wait_on);
  if (posix_wait_handle_to_signal == nullptr || posix_wait_handle_to_wait_on == nullptr) {
    return WaitResult::kFailed;
  }
  if (!posix_wait_handle_to_signal->GetCondition().Signal()) {
    return WaitResult::kFailed;
  }
  return Wait(wait_handle_to_wait_on, is_alertable, timeout);
}

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[], size_t wait_handle_count,
                                           bool wait_all, bool is_alertable,
                                           std::chrono::milliseconds timeout) {
  std::vector<PosixConditionBase*> conditions;
  conditions.reserve(wait_handle_count);
  for (size_t i = 0u; i < wait_handle_count; ++i) {
    auto handle = dynamic_cast<PosixWaitHandle*>(wait_handles[i]);
    if (handle == nullptr) {
      return std::make_pair(WaitResult::kFailed, 0);
    }
    conditions.push_back(&handle->GetCondition());
  }
  if (!is_alertable && conditions.size() == 1) {
    return std::make_pair(conditions[0]->Wait(timeout), size_t{0});
  }
  ScopedAlertableState alertable_state_guard(is_alertable);
  return PosixConditionBase::WaitMultiple(conditions, wait_all, is_alertable,
                                          MakeDeadline(timeout));
}

class PosixEvent : public PosixConditionHandle<Event> {
 public:
  PosixEvent(bool manual_reset, bool initial_state)
      : PosixConditionHandle(manual_reset, initial_state) {}
  ~PosixEvent() override = default;
  void Set() override { handle_.Signal(); }
  void Reset() override { handle_.Reset(); }
  void Pulse() override {
    using namespace std::chrono_literals;
    handle_.Signal();
    MaybeYield();
    Sleep(10us);
    handle_.Reset();
  }
};

std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(true, initial_state);
}

std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(false, initial_state);
}

class PosixSemaphore : public PosixConditionHandle<Semaphore> {
 public:
  PosixSemaphore(int initial_count, int maximum_count)
      : PosixConditionHandle(initial_count, maximum_count) {}
  ~PosixSemaphore() override = default;
  bool Release(int release_count, int* out_previous_count) override {
    if (release_count < 1) {
      return false;
    }
    return handle_.Release(release_count, out_previous_count);
  }
};

std::unique_ptr<Semaphore> Semaphore::Create(int initial_count, int maximum_count) {
  if (initial_count < 0 || initial_count > maximum_count || maximum_count <= 0) {
    return nullptr;
  }
  return std::make_unique<PosixSemaphore>(initial_count, maximum_count);
}

class PosixMutant : public PosixConditionHandle<Mutant> {
 public:
  explicit PosixMutant(bool initial_owner) : PosixConditionHandle(initial_owner) {}
  ~PosixMutant() override = default;
  bool Release() override { return handle_.Release(); }
};

std::unique_ptr<Mutant> Mutant::Create(bool initial_owner) {
  return std::make_unique<PosixMutant>(initial_owner);
}

class PosixTimer : public PosixConditionHandle<Timer> {
  using WClock = Timer::WindowsClock;
  using GClock = Timer::SteadyClock;

 public:
  explicit PosixTimer(bool manual_reset) : PosixConditionHandle(manual_reset) {}
  ~PosixTimer() override = default;

  bool SetOnceAfter(rex::HundredNanoseconds rel_time,
                    std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(GClock::now() + rel_time, std::move(opt_callback));
  }
  bool SetOnceAt(WClock::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(std::chrono::clock_cast<GClock>(due_time), std::move(opt_callback));
  };
  bool SetOnceAt(GClock::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    handle_.SetOnce(due_time, std::move(opt_callback));
    return true;
  }

  bool SetRepeatingAfter(rex::HundredNanoseconds rel_time, std::chrono::milliseconds period,
                         std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(GClock::now() + rel_time, period, std::move(opt_callback));
  }
  bool SetRepeatingAt(WClock::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(std::chrono::clock_cast<GClock>(due_time), period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(GClock::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    handle_.SetRepeating(due_time, period, std::move(opt_callback));
    return true;
  }
  bool Cancel() override {
    handle_.Cancel();
    return true;
  }
  bool Reset() override {
    handle_.Reset();
    return true;
  }
};

std::unique_ptr<Timer> Timer::CreateManualResetTimer() {
  return std::make_unique<PosixTimer>(true);
}

std::unique_ptr<Timer> Timer::CreateSynchronizationTimer() {
  return std::make_unique<PosixTimer>(false);
}

class PosixThread : public PosixConditionHandle<Thread> {
 public:
  PosixThread() = default;
  explicit PosixThread(pthread_t thread) : PosixConditionHandle(thread) {}
  ~PosixThread() override = default;

  bool Initialize(CreationParameters params, rex::MoveOnlyFunction<void()> start_routine) {
    auto start_data = std::make_unique<ThreadStartArgs>(std::move(start_routine), false, this);
    return handle_.Initialize(params, std::move(start_data));
  }

  void SetName(std::string name) override {
    handle_.WaitStarted();
    Thread::SetName(name);
    if (name.length() > 15) {
      name = name.substr(0, 15);
    }
    handle_.SetName(name);
  }

  u32 GetSystemId() const override { return handle_.GetSystemId(); }

  u64 GetAffinityMask() override { return handle_.GetAffinityMask(); }
  void SetAffinityMask(u64 mask) override { handle_.SetAffinityMask(mask); }

  int GetPriority() override { return handle_.GetPriority(); }
  void SetPriority(int new_priority) override { handle_.SetPriority(new_priority); }

  void QueueUserCallback(rex::MoveOnlyFunction<void()> callback) override {
    handle_.QueueUserCallback(std::move(callback));
  }

  bool Resume(u32* out_previous_suspend_count) override {
    return handle_.Resume(out_previous_suspend_count);
  }

  bool Suspend(u32* out_previous_suspend_count) override {
    return handle_.Suspend(out_previous_suspend_count);
  }

  bool Redirect(RedirectFilter filter, void* context, void (*target)()) override {
    return handle_.Redirect(filter, context, target);
  }

  [[noreturn]] void ExitCurrent(int exit_code) { handle_.ExitCurrent(exit_code); }

  void WaitSuspended() { handle_.WaitSuspended(); }

  bool SuspendIf(RedirectFilter filter, void* context) override {
    return handle_.SuspendIf(filter, context);
  }

  void HandleSuspend(void* signal_context) { handle_.HandleSuspend(signal_context); }

  void HandleRedirect(int signal, void* signal_context) {
    handle_.HandleRedirect(signal, signal_context);
  }
};

thread_local PosixThread* g_current_thread = nullptr;
thread_local std::unique_ptr<PosixThread> g_owned_current_thread;

void* PosixCondition<Thread>::ThreadStartRoutine(void* parameter) {
#if !REX_PLATFORM_ANDROID
  if (pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, nullptr) != 0) {
    REX_ASSERT_ALWAYS();
  }
#endif
  SetCurrentThreadName("");
  detail::RegisterCurrentThread();
  PrepareThreadForCrash();

  std::unique_ptr<ThreadStartArgs> start_data(static_cast<ThreadStartArgs*>(parameter));
  REX_ASSERT(start_data != nullptr);
  REX_ASSERT(start_data->thread_obj != nullptr);

  auto thread = dynamic_cast<PosixThread*>(start_data->thread_obj);
  auto start_routine = std::move(start_data->start_routine);
  auto create_suspended = start_data->create_suspended;
  start_data.reset();

  g_current_thread = thread;
  g_current_thread_condition = &thread->handle_;
  {
    std::unique_lock lock(thread->handle_.state_mutex_);
    thread->handle_.state_ = create_suspended ? State::kSuspended : State::kRunning;
    // Armed with the started state, or Resume sees suspend_count_ 0 and the wait never ends.
    if (create_suspended) {
      thread->handle_.suspend_count_ = 1;
    }
    thread->handle_.state_signal_.notify_all();
  }

  if (create_suspended) {
    std::unique_lock lock(thread->handle_.state_mutex_);
    thread->handle_.state_mutex_.Wait(thread->handle_.state_signal_,
                                      [thread] { return thread->handle_.suspend_count_ == 0; });
  }

  start_routine();

  PosixCondition<Mutant>::AbandonOwnedBy(pthread_self());
  {
    std::unique_lock lock(thread->handle_.state_mutex_);
    thread->handle_.state_ = State::kFinished;
  }

  thread->handle_.SignalExit(0);

  g_current_thread = nullptr;
  g_current_thread_condition = nullptr;
  return nullptr;
}

std::unique_ptr<Thread> Thread::Create(CreationParameters params,
                                       rex::MoveOnlyFunction<void()> start_routine) {
  InstallSignalHandler(SignalType::kThreadSuspend);
  InstallSignalHandler(SignalType::kThreadUserCallback);
  auto thread = std::make_unique<PosixThread>();
  if (!thread->Initialize(params, std::move(start_routine)))
    return nullptr;
  REX_ASSERT(thread != nullptr);
  return thread;
}

Thread* Thread::GetCurrentThread() {
  if (g_current_thread) {
    return g_current_thread;
  }

  // Threads not created by Thread::Create need the process wide handlers for suspend and APC.
  InstallSignalHandler(SignalType::kThreadSuspend);
  InstallSignalHandler(SignalType::kThreadUserCallback);

  pthread_t handle = pthread_self();

  g_owned_current_thread = std::make_unique<PosixThread>(handle);
  g_current_thread = g_owned_current_thread.get();
  g_current_thread_condition = &g_current_thread->GetCondition();

  return g_current_thread;
}

void Thread::Exit(int exit_code) {
  if (g_current_thread) {
    g_current_thread->ExitCurrent(exit_code);
  } else {
    // No current_thread means the thread was not created by Thread::Create.
    // NOLINTNEXTLINE(performance-no-int-to-ptr): pthread passes the value as a void pointer
    pthread_exit(reinterpret_cast<void*>(exit_code));
  }
  REX_ASSERT_ALWAYS();
}

void SetCurrentThreadName(const std::string_view name) {
#if defined(__APPLE__)
  pthread_setname_np(std::string(name).c_str());
#else
  pthread_setname_np(pthread_self(), std::string(name).c_str());
#endif
#if REX_PLATFORM_ANDROID
  if (!g_android_pthread_getname_np && current_thread) {
    current_thread->GetCondition().SetAndroidPreAPI26Name(name);
  }
#endif
}

static void SignalHandler(int signal, siginfo_t* /*info*/, void* context) {
  switch (GetSystemSignalType(signal)) {
    case SignalType::kThreadSuspend: {
      if (!g_current_thread) {
        return;
      }
      g_current_thread->HandleSuspend(context);
    } break;
    case SignalType::kThreadUserCallback: {
      if (g_current_thread) {
        g_current_thread->HandleRedirect(signal, context);
      }
    } break;
    default:
      REX_ASSERT_ALWAYS();
  }
}

}  // namespace rex::platform
