/**
 * @file        core/threading_posix.cpp
 * @brief       POSIX platform threading implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>
#include <rex/thread.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <signal.h>

#include <atomic>
#include <array>
#include <cerrno>
#include <cstddef>
#include <ctime>
#include <deque>
#include <limits>
#include <memory>

#include <pthread.h>
#include <semaphore.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <rex/assert.h>
#include <rex/chrono/chrono_steady_cast.h>
#include <rex/logging.h>
#include <rex/thread/timer_queue.h>

#include <sched.h>

#if REX_PLATFORM_ANDROID
#include <dlfcn.h>

#include <rex/main_android.h>
#include <rex/string/util.h>
#endif

#if REX_PLATFORM_LINUX
// SIGEV_THREAD_ID in timer_create(...) is a Linux extension
#define REX_HAS_SIGEV_THREAD_ID 1
#ifdef __GLIBC__
#define sigev_notify_thread_id _sigev_un._tid
#endif
#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
#define gettid() syscall(SYS_gettid)
#endif
#else
#define REX_HAS_SIGEV_THREAD_ID 0
#endif

namespace rex::thread {

#if REX_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* android_libc_;
// API 26+.
static int (*android_pthread_getname_np_)(pthread_t pthread, char* buf, size_t n);

void AndroidInitialize() {
  if (rex::GetAndroidApiLevel() >= 26) {
    android_libc_ = dlopen("libc.so", RTLD_NOW);
    assert_not_null(android_libc_);
    if (android_libc_) {
      android_pthread_getname_np_ = reinterpret_cast<decltype(android_pthread_getname_np_)>(
          dlsym(android_libc_, "pthread_getname_np"));
      assert_not_null(android_pthread_getname_np_);
    }
  }
}

void AndroidShutdown() {
  android_pthread_getname_np_ = nullptr;
  if (android_libc_) {
    dlclose(android_libc_);
    android_libc_ = nullptr;
  }
}
#endif

template <typename _Rep, typename _Period>
inline timespec DurationToTimeSpec(std::chrono::duration<_Rep, _Period> duration) {
  auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
  auto div = ldiv(nanoseconds.count(), 1000000000L);
  return timespec{div.quot, div.rem};
}

// Thread interruption is done using user-defined signals
// This implementation uses the SIGRTMAX - SIGRTMIN to signal to a thread
// gdb tip, for SIG = SIGRTMIN + SignalType : handle SIG nostop
// lldb tip, for SIG = SIGRTMIN + SignalType : process handle SIG -s false
enum class SignalType {
  kThreadSuspend,
  kThreadUserCallback,
#if REX_PLATFORM_ANDROID
  // pthread_cancel is not available on Android, using a signal handler for
  // simplified PTHREAD_CANCEL_ASYNCHRONOUS-like behavior - not disabling
  // cancellation currently, so should be enough.
  kThreadTerminate,
#endif
  k_Count
};

int GetSystemSignal(SignalType num) {
  auto result = SIGRTMIN + static_cast<int>(num);
  assert_true(result < SIGRTMAX);
  return result;
}

SignalType GetSystemSignalType(int num) {
  return static_cast<SignalType>(num - SIGRTMIN);
}

std::array<std::atomic<bool>, static_cast<size_t>(SignalType::k_Count)> signal_handler_installed =
    {};

static void signal_handler(int signal, siginfo_t* info, void* context);

void install_signal_handler(SignalType type) {
  bool expected = false;
  if (!signal_handler_installed[static_cast<size_t>(type)].compare_exchange_strong(expected,
                                                                                   true)) {
    return;
  }
  struct sigaction action{};
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  action.sa_sigaction = signal_handler;
  sigemptyset(&action.sa_mask);
  if (sigaction(GetSystemSignal(type), &action, nullptr) != 0) {
    signal_handler_installed[static_cast<size_t>(type)] = false;
  }
}

// TODO(dougvj)
void EnableAffinityConfiguration() {}

// uint64_t ticks() { return mach_absolute_time(); }

uint32_t current_thread_system_id() {
  return static_cast<uint32_t>(syscall(SYS_gettid));
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
    // Swap requested for remaining in case of signal interruption
    // in which case, we start sleeping again for the remainder
    std::swap(p_rqtp, p_rmtp);
  } while (ret == -1 && errno == EINTR);
}

// TODO(bwrsandman) Implement by allowing alert interrupts from IO operations
thread_local bool alertable_state_ = false;
bool DispatchCurrentThreadUserCallback();
SleepResult AlertableSleep(std::chrono::microseconds duration) {
  alertable_state_ = true;
  auto deadline = std::chrono::steady_clock::now() + duration;
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      alertable_state_ = false;
      return SleepResult::kAlerted;
    }
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      alertable_state_ = false;
      return SleepResult::kSuccess;
    }
    auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
    Sleep(std::min(remaining, std::chrono::microseconds(1000)));
  }
}

TlsHandle AllocateTlsHandle() {
  auto key = static_cast<pthread_key_t>(-1);
  auto res = pthread_key_create(&key, nullptr);
  assert_zero(res);
  assert_true(key != static_cast<pthread_key_t>(-1));
  return static_cast<TlsHandle>(key);
}

bool FreeTlsHandle(TlsHandle handle) {
  return pthread_key_delete(static_cast<pthread_key_t>(handle)) == 0;
}

uintptr_t GetTlsValue(TlsHandle handle) {
  return reinterpret_cast<uintptr_t>(pthread_getspecific(static_cast<pthread_key_t>(handle)));
}

bool SetTlsValue(TlsHandle handle, uintptr_t value) {
  return pthread_setspecific(static_cast<pthread_key_t>(handle), reinterpret_cast<void*>(value)) ==
         0;
}

class PosixConditionBase {
 public:
  PosixConditionBase() {
#if REX_PLATFORM_LINUX
    // Use robust mutexes so waits can recover if owner thread terminates.
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
    auto predicate = [this] { return this->signaled(); };
#if REX_PLATFORM_LINUX
    auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
    int lock_result = pthread_mutex_lock(native_mutex);
    if (lock_result == EOWNERDEAD) {
      pthread_mutex_consistent(native_mutex);
    } else if (lock_result != 0) {
      return WaitResult::kFailed;
    }
    std::unique_lock<std::mutex> lock(mutex_, std::adopt_lock);
#else
    std::unique_lock<std::mutex> lock(mutex_);
#endif
    if (predicate()) {
      executed = true;
    } else {
      if (timeout == std::chrono::milliseconds::max()) {
        cond_.wait(lock, predicate);
        executed = true;  // Did not time out;
      } else {
        executed = cond_.wait_for(lock, timeout, predicate);
      }
    }
    if (executed) {
      post_execution();
      return WaitResult::kSuccess;
    } else {
      return WaitResult::kTimeout;
    }
  }

  static std::pair<WaitResult, size_t> WaitMultiple(std::vector<PosixConditionBase*>&& handles,
                                                    bool wait_all,
                                                    std::chrono::milliseconds timeout) {
    assert_true(!handles.empty());

    if (handles.size() == 1) {
      auto result = handles[0]->Wait(timeout);
      return std::make_pair(result, 0);
    }

    auto start_time = std::chrono::steady_clock::now();
    auto end_time = (timeout == std::chrono::milliseconds::max())
                        ? std::chrono::steady_clock::time_point::max()
                        : start_time + timeout;

    while (true) {
      size_t first_signaled = std::numeric_limits<size_t>::max();
      bool condition_met = false;
      bool all_locked = true;

      std::vector<std::unique_lock<std::mutex>> locks;
      locks.reserve(handles.size());

      for (size_t i = 0; i < handles.size(); ++i) {
#if REX_PLATFORM_LINUX
        auto native_mutex = static_cast<pthread_mutex_t*>(handles[i]->mutex_.native_handle());
        int result = pthread_mutex_trylock(native_mutex);
        if (result == 0 || result == EOWNERDEAD) {
          if (result == EOWNERDEAD) {
            pthread_mutex_consistent(native_mutex);
          }
          locks.emplace_back(handles[i]->mutex_, std::adopt_lock);
        } else {
          all_locked = false;
          break;
        }
#else
        locks.emplace_back(handles[i]->mutex_, std::try_to_lock);
        if (!locks.back().owns_lock()) {
          all_locked = false;
          break;
        }
#endif
      }

      if (!all_locked) {
        locks.clear();
        std::this_thread::yield();
        continue;
      }

      if (wait_all) {
        bool all_signaled = true;
        for (size_t i = 0; i < handles.size(); ++i) {
          if (!handles[i]->signaled()) {
            all_signaled = false;
            break;
          }
          if (first_signaled == std::numeric_limits<size_t>::max()) {
            first_signaled = i;
          }
        }
        condition_met = all_signaled;
      } else {
        for (size_t i = 0; i < handles.size(); ++i) {
          if (handles[i]->signaled()) {
            first_signaled = i;
            condition_met = true;
            break;
          }
        }
      }

      if (condition_met) {
        if (wait_all) {
          for (size_t i = 0; i < handles.size(); ++i) {
            handles[i]->post_execution();
          }
        } else {
          handles[first_signaled]->post_execution();
        }
        return std::make_pair(WaitResult::kSuccess, first_signaled);
      }

      locks.clear();

      auto now = std::chrono::steady_clock::now();
      if (now >= end_time) {
        return std::make_pair<WaitResult, size_t>(WaitResult::kTimeout, 0);
      }

      if (timeout == std::chrono::milliseconds::max()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } else {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - now);
        auto sleep_time = std::min(remaining, std::chrono::milliseconds(1));
        std::this_thread::sleep_for(sleep_time);
      }
    }
  }

  virtual void* native_handle() const {
    return const_cast<std::condition_variable&>(cond_).native_handle();
  }

 protected:
  inline virtual bool signaled() const = 0;
  inline virtual void post_execution() = 0;
  std::condition_variable cond_;
  std::mutex mutex_;
};

// There really is no native POSIX handle for a single wait/signal construct
// pthreads is at a lower level with more handles for such a mechanism.
// This simple wrapper class functions as our handle and uses conditional
// variables for waits and signals.
template <typename T>
class PosixCondition {};

template <>
class PosixCondition<Event> : public PosixConditionBase {
 public:
  PosixCondition(bool manual_reset, bool initial_state)
      : signal_(initial_state), manual_reset_(manual_reset) {}
  virtual ~PosixCondition() = default;

  bool Signal() override {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    signal_ = true;
    cond_.notify_all();
    return true;
  }

  void Reset() {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    signal_ = false;
  }

 private:
  inline bool signaled() const override { return signal_; }
  inline void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  bool signal_;
  const bool manual_reset_;
};

template <>
class PosixCondition<Semaphore> : public PosixConditionBase {
 public:
  PosixCondition(uint32_t initial_count, uint32_t maximum_count)
      : count_(initial_count), maximum_count_(maximum_count) {}

  bool Signal() override { return Release(1, nullptr); }

  bool Release(uint32_t release_count, int* out_previous_count) {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    if (release_count > maximum_count_ - count_) {
      return false;
    }
    if (out_previous_count) {
      *out_previous_count = count_;
    }
    count_ += release_count;
    cond_.notify_all();
    return true;
  }

 private:
  inline bool signaled() const override { return count_ > 0; }
  inline void post_execution() override {
    count_--;
    cond_.notify_all();
  }
  uint32_t count_;
  const uint32_t maximum_count_;
};

template <>
class PosixCondition<Mutant> : public PosixConditionBase {
 public:
  explicit PosixCondition(bool initial_owner) : count_(0) {
    if (initial_owner) {
      count_ = 1;
      owner_ = std::this_thread::get_id();
    }
  }

  bool Signal() override { return Release(); }

  bool Release() {
    if (owner_ == std::this_thread::get_id() && count_ > 0) {
      auto lock = std::unique_lock<std::mutex>(mutex_);
      --count_;
      // Free to be acquired by another thread
      if (count_ == 0) {
        cond_.notify_all();
      }
      return true;
    }
    return false;
  }

  void* native_handle() const override { return const_cast<std::mutex&>(mutex_).native_handle(); }

 private:
  inline bool signaled() const override {
    return count_ == 0 || owner_ == std::this_thread::get_id();
  }
  inline void post_execution() override {
    count_++;
    owner_ = std::this_thread::get_id();
  }
  uint32_t count_;
  std::thread::id owner_;
};

template <>
class PosixCondition<Timer> : public PosixConditionBase {
 public:
  explicit PosixCondition(bool manual_reset)
      : callback_(nullptr), signal_(false), manual_reset_(manual_reset) {}

  virtual ~PosixCondition() { Cancel(); }

  bool Signal() override {
    std::lock_guard<std::mutex> lock(mutex_);
    signal_ = true;
    cond_.notify_all();
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

  void* native_handle() const override {
    assert_always();
    return nullptr;
  }

 private:
  static void CompletionRoutine(void* userdata) {
    assert_not_null(userdata);
    auto timer = reinterpret_cast<PosixCondition<Timer>*>(userdata);
    timer->Signal();
    // As the callback may reset the timer, store local.
    std::function<void()> callback;
    {
      std::lock_guard<std::mutex> lock(timer->mutex_);
      callback = timer->callback_;
    }
    if (callback) {
      callback();
    }
  }

 private:
  inline bool signaled() const override { return signal_; }
  inline void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  std::weak_ptr<TimerQueueWaitItem> wait_item_;
  std::function<void()> callback_;
  bool signal_;  // Protected by mutex_
  const bool manual_reset_;
};

struct ThreadStartData {
  std::function<void()> start_routine;
  bool create_suspended;
  Thread* thread_obj;
};

template <>
class PosixCondition<Thread> : public PosixConditionBase {
  enum class State {
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
    sem_init(&suspend_sem_, 0, 0);
#if REX_PLATFORM_ANDROID
    android_pre_api_26_name_[0] = '\0';
#endif
  }
  bool Initialize(Thread::CreationParameters params, ThreadStartData* start_data) {
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
    if (pthread_create(&thread_, &attr, ThreadStartRoutine, start_data) != 0) {
      pthread_attr_destroy(&attr);
      return false;
    }
    pthread_attr_destroy(&attr);
    return true;
  }

  /// Constructor for existing thread. This should only happen once called by
  /// Thread::GetCurrentThread() on the main thread
  explicit PosixCondition(pthread_t thread)
      : thread_(thread),
        signaled_(false),
        exit_code_(0),
        state_(State::kRunning),
        suspend_count_(0) {
    sem_init(&suspend_sem_, 0, 0);
#if REX_PLATFORM_ANDROID
    android_pre_api_26_name_[0] = '\0';
#endif
  }

  virtual ~PosixCondition() {
    // Match Canary/Edge behavior.
    // Force-cancel/join from the condition destructor can self-join/crash
    // depending on shutdown ordering, so threads must be stopped explicitly.
  }

  bool Signal() override { return true; }

  std::string name() const {
    WaitStarted();
    auto result = std::array<char, 17>{'\0'};
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (state_ != State::kUninitialized && state_ != State::kFinished) {
#if REX_PLATFORM_ANDROID
      // pthread_getname_np was added in API 26 - below that, store the name in
      // this object, which may be only modified through Xenia threading, but
      // should be enough in most cases.
      if (android_pthread_getname_np_) {
        if (android_pthread_getname_np_(thread_, result.data(), result.size() - 1) != 0) {
          assert_always();
        }
      } else {
        std::lock_guard<std::mutex> lock(android_pre_api_26_name_mutex_);
        std::strcpy(result.data(), android_pre_api_26_name_);
      }
#else
      if (pthread_getname_np(thread_, result.data(), result.size() - 1) != 0) {
        assert_always();
      }
#endif
    }
    return std::string(result.data());
  }

  void set_name(const std::string& name) {
    WaitStarted();
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (state_ != State::kUninitialized && state_ != State::kFinished) {
      pthread_setname_np(thread_, std::string(name).c_str());
#if REX_PLATFORM_ANDROID
      SetAndroidPreApi26Name(name);
#endif
    }
  }

#if REX_PLATFORM_ANDROID
  void SetAndroidPreApi26Name(const std::string_view name) {
    if (android_pthread_getname_np_) {
      return;
    }
    std::lock_guard<std::mutex> lock(android_pre_api_26_name_mutex_);
    rex::string::util_copy_truncating(android_pre_api_26_name_, name,
                                      rex::countof(android_pre_api_26_name_));
  }
#endif

  uint32_t system_id() const { return static_cast<uint32_t>(thread_); }

  uint64_t affinity_mask() {
    WaitStarted();
    cpu_set_t cpu_set;
#if REX_PLATFORM_ANDROID
    if (sched_getaffinity(pthread_gettid_np(thread_), sizeof(cpu_set_t), &cpu_set) != 0) {
      assert_always();
    }
#else
    if (pthread_getaffinity_np(thread_, sizeof(cpu_set_t), &cpu_set) != 0) {
      assert_always();
    }
#endif
    uint64_t result = 0;
    auto cpu_count = std::min(CPU_SETSIZE, 64);
    for (auto i = 0u; i < cpu_count; i++) {
      auto set = CPU_ISSET(i, &cpu_set);
      result |= set << i;
    }
    return result;
  }

  void set_affinity_mask(uint64_t mask) {
    WaitStarted();
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    for (auto i = 0u; i < 64; i++) {
      if (mask & (1ULL << i)) {
        CPU_SET(i, &cpu_set);
      }
    }
#if REX_PLATFORM_ANDROID
    if (sched_setaffinity(pthread_gettid_np(thread_), sizeof(cpu_set_t), &cpu_set) != 0) {
      assert_always();
    }
#else
    if (pthread_setaffinity_np(thread_, sizeof(cpu_set_t), &cpu_set) != 0) {
      assert_always();
    }
#endif
  }

  int priority() {
    WaitStarted();
    int policy;
    sched_param param{};
    int ret = pthread_getschedparam(thread_, &policy, &param);
    if (ret != 0) {
      return -1;
    }

    return param.sched_priority;
  }

  void set_priority(int new_priority) {
    WaitStarted();
    sched_param param{};
    param.sched_priority = new_priority;
    int result = pthread_setschedparam(thread_, SCHED_FIFO, &param);
    if (result != 0) {
      switch (result) {
        case EPERM:
          REXSYS_WARN("set_priority: permission denied");
          break;
        case EINVAL:
          assert_always();
          break;
        default:
          REXSYS_WARN("set_priority: pthread_setschedparam failed ({})", result);
          break;
      }
    }
  }

  void QueueUserCallback(std::function<void()> callback) {
    WaitStarted();
    {
      std::unique_lock<std::mutex> lock(callback_mutex_);
      user_callbacks_.push_back(std::move(callback));
      has_pending_user_callbacks_.store(true, std::memory_order_release);
    }

    // If the callback is queued on the current thread, don't self-signal.
    // Alertable waits drain this queue in normal thread context.
    if (pthread_equal(thread_, pthread_self())) {
      if (alertable_state_) {
        DispatchQueuedUserCallbacks();
      }
      return;
    }

    sigval value{};
    value.sival_ptr = this;
#if REX_PLATFORM_ANDROID
    int result = sigqueue(pthread_gettid_np(thread_),
                          GetSystemSignal(SignalType::kThreadUserCallback), value);
#else
    int result = pthread_sigqueue(thread_, GetSystemSignal(SignalType::kThreadUserCallback), value);
#endif
    if (result != 0) {
      REXSYS_WARN("QueueUserCallback: signal delivery failed ({})", result);
    }
  }

  bool DispatchQueuedUserCallbacks() {
    if (!has_pending_user_callbacks_.load(std::memory_order_acquire)) {
      return false;
    }

    std::function<void()> callback;
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

  bool Resume(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (suspend_count_ == 0) {
      return false;
    }
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = suspend_count_;
    }
    --suspend_count_;
    if (suspend_count_ == 0 && state_ == State::kSuspended) {
      state_ = State::kRunning;
      // Async-signal-safe wakeup for WaitSuspended() from signal handler path.
      sem_post(&suspend_sem_);
    }
    state_signal_.notify_all();
    return true;
  }

  bool Suspend(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    bool is_current_thread = pthread_self() == thread_;
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (out_previous_suspend_count) {
        *out_previous_suspend_count = suspend_count_;
      }
      state_ = State::kSuspended;
      ++suspend_count_;
    }

    if (is_current_thread) {
      WaitSuspended();
      return true;
    }

    int result = pthread_kill(thread_, GetSystemSignal(SignalType::kThreadSuspend));
    return result == 0;
  }

  void Terminate(int exit_code) {
    bool is_current_thread = pthread_self() == thread_;
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (state_ == State::kFinished) {
        if (is_current_thread) {
          // This is really bad. Some thread must have called Terminate() on us
          // just before we decided to terminate ourselves
          assert_always();
          for (;;) {
            // Wait for pthread_cancel() to actually happen.
          }
        }
        return;
      }
      state_ = State::kFinished;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);

      exit_code_ = exit_code;
      signaled_ = true;
      cond_.notify_all();
    }
    if (is_current_thread) {
      pthread_exit(reinterpret_cast<void*>(exit_code));
    } else {
#if REX_PLATFORM_ANDROID
      if (pthread_kill(thread_, GetSystemSignal(SignalType::kThreadTerminate)) != 0) {
        assert_always();
      }
#else
      if (pthread_cancel(thread_) != 0) {
        assert_always();
      }
#endif
    }
  }

  void WaitStarted() const {
    std::unique_lock<std::mutex> lock(state_mutex_);
    state_signal_.wait(lock, [this] { return state_ != State::kUninitialized; });
  }

  /// Uses sem_wait because it may be called from signal handler context.
  void WaitSuspended() {
    int ret;
    do {
      ret = sem_wait(&suspend_sem_);
    } while (ret == -1 && errno == EINTR);
  }

  void* native_handle() const override { return reinterpret_cast<void*>(thread_); }

 private:
  static void* ThreadStartRoutine(void* parameter);
  inline bool signaled() const override { return signaled_; }
  inline void post_execution() override {
    if (thread_) {
      pthread_join(thread_, nullptr);
    }
    sem_destroy(&suspend_sem_);
  }
  pthread_t thread_;
  bool signaled_;
  int exit_code_;
  State state_;             // Protected by state_mutex_
  uint32_t suspend_count_;  // Protected by state_mutex_
  sem_t suspend_sem_;
  mutable std::mutex state_mutex_;
  mutable std::mutex callback_mutex_;
  mutable std::condition_variable state_signal_;
  std::deque<std::function<void()>> user_callbacks_;
  std::atomic<bool> has_pending_user_callbacks_{false};
#if REX_PLATFORM_ANDROID
  // Name accessible via name() on Android before API 26 which added
  // pthread_getname_np.
  mutable std::mutex android_pre_api_26_name_mutex_;
  char android_pre_api_26_name_[16];
#endif
};

class PosixWaitHandle {
 public:
  virtual ~PosixWaitHandle();
  virtual PosixConditionBase& condition() = 0;
};

PosixWaitHandle::~PosixWaitHandle() = default;

thread_local PosixCondition<Thread>* current_thread_condition_ = nullptr;

bool DispatchCurrentThreadUserCallback() {
  if (!current_thread_condition_) {
    Thread::GetCurrentThread();
  }
  return current_thread_condition_ && current_thread_condition_->DispatchQueuedUserCallbacks();
}

namespace {

constexpr auto kAlertablePollSlice = std::chrono::milliseconds(1);

class ScopedAlertableState {
 public:
  explicit ScopedAlertableState(bool alertable) : alertable_(alertable) {
    if (alertable_) {
      alertable_state_ = true;
    }
  }
  ~ScopedAlertableState() {
    if (alertable_) {
      alertable_state_ = false;
    }
  }

 private:
  bool alertable_;
};

std::chrono::steady_clock::time_point ComputeAlertableDeadline(std::chrono::milliseconds timeout) {
  if (timeout == std::chrono::milliseconds::max()) {
    return std::chrono::steady_clock::time_point::max();
  }
  return std::chrono::steady_clock::now() + timeout;
}

bool HasAlertableTimeoutElapsed(std::chrono::steady_clock::time_point deadline) {
  return deadline != std::chrono::steady_clock::time_point::max() &&
         std::chrono::steady_clock::now() >= deadline;
}

std::chrono::milliseconds ComputeAlertableWaitTimeout(
    std::chrono::steady_clock::time_point deadline) {
  if (deadline == std::chrono::steady_clock::time_point::max()) {
    return kAlertablePollSlice;
  }
  auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  if (remaining <= std::chrono::milliseconds::zero()) {
    return std::chrono::milliseconds::zero();
  }
  return std::min(remaining, kAlertablePollSlice);
}

}  // namespace

// This wraps a condition object as our handle because posix has no single
// native handle for higher level concurrency constructs such as semaphores
template <typename T>
class PosixConditionHandle : public T, public PosixWaitHandle {
 public:
  PosixConditionHandle() = default;
  explicit PosixConditionHandle(bool);
  explicit PosixConditionHandle(pthread_t thread);
  PosixConditionHandle(bool manual_reset, bool initial_state);
  PosixConditionHandle(uint32_t initial_count, uint32_t maximum_count);
  ~PosixConditionHandle() override = default;

  PosixCondition<T>& condition() override { return handle_; }
  void* native_handle() const override { return handle_.native_handle(); }

 protected:
  PosixCondition<T> handle_;
  friend PosixCondition<T>;
};

template <>
PosixConditionHandle<Semaphore>::PosixConditionHandle(uint32_t initial_count,
                                                      uint32_t maximum_count)
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
    return posix_wait_handle->condition().Wait(timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  auto deadline = ComputeAlertableDeadline(timeout);

  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return WaitResult::kUserCallback;
    }
    if (HasAlertableTimeoutElapsed(deadline)) {
      return WaitResult::kTimeout;
    }
    auto result = posix_wait_handle->condition().Wait(ComputeAlertableWaitTimeout(deadline));
    if (result != WaitResult::kTimeout) {
      return result;
    }
  }
}

WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal, WaitHandle* wait_handle_to_wait_on,
                         bool is_alertable, std::chrono::milliseconds timeout) {
  auto result = WaitResult::kFailed;
  auto posix_wait_handle_to_signal = dynamic_cast<PosixWaitHandle*>(wait_handle_to_signal);
  auto posix_wait_handle_to_wait_on = dynamic_cast<PosixWaitHandle*>(wait_handle_to_wait_on);
  if (posix_wait_handle_to_signal == nullptr || posix_wait_handle_to_wait_on == nullptr) {
    return WaitResult::kFailed;
  }
  if (!posix_wait_handle_to_signal->condition().Signal()) {
    return WaitResult::kFailed;
  }

  if (!is_alertable) {
    return posix_wait_handle_to_wait_on->condition().Wait(timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  auto deadline = ComputeAlertableDeadline(timeout);
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return WaitResult::kUserCallback;
    }
    if (HasAlertableTimeoutElapsed(deadline)) {
      return WaitResult::kTimeout;
    }
    result = posix_wait_handle_to_wait_on->condition().Wait(ComputeAlertableWaitTimeout(deadline));
    if (result != WaitResult::kTimeout) {
      return result;
    }
  }
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
    conditions.push_back(&handle->condition());
  }
  if (!is_alertable) {
    return PosixConditionBase::WaitMultiple(std::move(conditions), wait_all, timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  auto deadline = ComputeAlertableDeadline(timeout);
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return std::make_pair(WaitResult::kUserCallback, 0);
    }
    if (HasAlertableTimeoutElapsed(deadline)) {
      return std::make_pair(WaitResult::kTimeout, 0);
    }
    auto result = PosixConditionBase::WaitMultiple(std::vector<PosixConditionBase*>(conditions),
                                                   wait_all, ComputeAlertableWaitTimeout(deadline));
    if (result.first != WaitResult::kTimeout) {
      return result;
    }
  }
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
      : PosixConditionHandle(static_cast<uint32_t>(initial_count),
                             static_cast<uint32_t>(maximum_count)) {}
  ~PosixSemaphore() override = default;
  bool Release(int release_count, int* out_previous_count) override {
    if (release_count < 1) {
      return false;
    }
    return handle_.Release(static_cast<uint32_t>(release_count), out_previous_count);
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
  using WClock_ = Timer::WClock_;
  using GClock_ = Timer::GClock_;

 public:
  explicit PosixTimer(bool manual_reset) : PosixConditionHandle(manual_reset) {}
  ~PosixTimer() override = default;

  bool SetOnceAfter(rex::chrono::hundrednanoseconds rel_time,
                    std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(GClock_::now() + rel_time, std::move(opt_callback));
  }
  bool SetOnceAt(WClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(std::chrono::clock_cast<GClock_>(due_time), std::move(opt_callback));
  };
  bool SetOnceAt(GClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    handle_.SetOnce(due_time, std::move(opt_callback));
    return true;
  }

  bool SetRepeatingAfter(rex::chrono::hundrednanoseconds rel_time, std::chrono::milliseconds period,
                         std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(GClock_::now() + rel_time, period, std::move(opt_callback));
  }
  bool SetRepeatingAt(WClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(std::chrono::clock_cast<GClock_>(due_time), period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(GClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    handle_.SetRepeating(due_time, period, std::move(opt_callback));
    return true;
  }
  bool Cancel() override {
    handle_.Cancel();
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

  bool Initialize(CreationParameters params, std::function<void()> start_routine) {
    auto start_data = new ThreadStartData({std::move(start_routine), false, this});
    return handle_.Initialize(params, start_data);
  }

  void set_name(std::string name) override {
    handle_.WaitStarted();
    Thread::set_name(name);
    if (name.length() > 15) {
      name = name.substr(0, 15);
    }
    handle_.set_name(name);
  }

  uint32_t system_id() const override { return handle_.system_id(); }

  uint64_t affinity_mask() override { return handle_.affinity_mask(); }
  void set_affinity_mask(uint64_t mask) override { handle_.set_affinity_mask(mask); }

  int priority() override { return handle_.priority(); }
  void set_priority(int new_priority) override { handle_.set_priority(new_priority); }

  void QueueUserCallback(std::function<void()> callback) override {
    handle_.QueueUserCallback(std::move(callback));
  }

  bool Resume(uint32_t* out_previous_suspend_count) override {
    return handle_.Resume(out_previous_suspend_count);
  }

  bool Suspend(uint32_t* out_previous_suspend_count) override {
    return handle_.Suspend(out_previous_suspend_count);
  }

  void Terminate(int exit_code) override { handle_.Terminate(exit_code); }

  void WaitSuspended() { handle_.WaitSuspended(); }
};

thread_local PosixThread* current_thread_ = nullptr;

void* PosixCondition<Thread>::ThreadStartRoutine(void* parameter) {
#if !REX_PLATFORM_ANDROID
  if (pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, nullptr) != 0) {
    assert_always();
  }
#endif
  set_current_thread_name("");

  auto start_data = static_cast<ThreadStartData*>(parameter);
  assert_not_null(start_data);
  assert_not_null(start_data->thread_obj);

  auto thread = dynamic_cast<PosixThread*>(start_data->thread_obj);
  auto start_routine = std::move(start_data->start_routine);
  auto create_suspended = start_data->create_suspended;
  delete start_data;

  current_thread_ = thread;
  current_thread_condition_ = &thread->handle_;
  {
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
    thread->handle_.state_ = create_suspended ? State::kSuspended : State::kRunning;
    thread->handle_.state_signal_.notify_all();
  }

  if (create_suspended) {
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
    thread->handle_.suspend_count_ = 1;
    thread->handle_.state_signal_.wait(lock,
                                       [thread] { return thread->handle_.suspend_count_ == 0; });
  }

  start_routine();

  {
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
    thread->handle_.state_ = State::kFinished;
  }

  {
    std::unique_lock<std::mutex> lock(thread->handle_.mutex_);
    thread->handle_.exit_code_ = 0;
    thread->handle_.signaled_ = true;
    thread->handle_.cond_.notify_all();
  }

  current_thread_ = nullptr;
  current_thread_condition_ = nullptr;
  return nullptr;
}

std::unique_ptr<Thread> Thread::Create(CreationParameters params,
                                       std::function<void()> start_routine) {
  install_signal_handler(SignalType::kThreadSuspend);
  install_signal_handler(SignalType::kThreadUserCallback);
#if REX_PLATFORM_ANDROID
  install_signal_handler(SignalType::kThreadTerminate);
#endif
  auto thread = std::make_unique<PosixThread>();
  if (!thread->Initialize(params, std::move(start_routine)))
    return nullptr;
  assert_not_null(thread);
  return thread;
}

Thread* Thread::GetCurrentThread() {
  if (current_thread_) {
    return current_thread_;
  }

  // Threads not created by Thread::Create (typically main thread) still need
  // process-wide signal handlers used by suspend/APC machinery.
  install_signal_handler(SignalType::kThreadSuspend);
  install_signal_handler(SignalType::kThreadUserCallback);
#if REX_PLATFORM_ANDROID
  install_signal_handler(SignalType::kThreadTerminate);
#endif

  // Should take this route only for threads not created by Thread::Create.
  // The only thread not created by Thread::Create should be the main thread.
  pthread_t handle = pthread_self();

  current_thread_ = new PosixThread(handle);
  current_thread_condition_ = &current_thread_->condition();
  // TODO(bwrsandman): Disabling deleting thread_local current thread to prevent
  //                   assert in destructor. Since this is thread local, the
  //                   "memory leaking" is controlled.
  // atexit([] { delete current_thread_; });

  return current_thread_;
}

void Thread::Exit(int exit_code) {
  if (current_thread_) {
    current_thread_->Terminate(exit_code);
  } else {
    // Should only happen with the main thread
    pthread_exit(reinterpret_cast<void*>(exit_code));
  }
  // Function must not return
  assert_always();
}

void set_current_thread_name(const std::string_view name) {
  pthread_setname_np(pthread_self(), std::string(name).c_str());
#if REX_PLATFORM_ANDROID
  if (!android_pthread_getname_np_ && current_thread_) {
    current_thread_->condition().SetAndroidPreApi26Name(name);
  }
#endif
}

static void signal_handler(int signal, siginfo_t* /*info*/, void* /*context*/) {
  switch (GetSystemSignalType(signal)) {
    case SignalType::kThreadSuspend: {
      if (!current_thread_) {
        return;
      }
      current_thread_->WaitSuspended();
    } break;
    case SignalType::kThreadUserCallback: {
      // Callbacks are drained from alertable waits in normal thread context.
      // This signal is only used as a wakeup hint.
    } break;
#if REX_PLATFORM_ANDROID
    case SignalType::kThreadTerminate: {
      pthread_exit(reinterpret_cast<void*>(-1));
    } break;
#endif
    default:
      assert_always();
  }
}

}  // namespace rex::thread
