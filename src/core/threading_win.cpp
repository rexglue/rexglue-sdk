/**
 * @file        core/threading_win.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#include <rex/platform/thread.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include "platform_win.h"

#include <array>
#include <cstring>
#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/chrono/chrono_steady_cast.h>
#include <rex/move_only_function.h>

#define REX_LOG_LAST_ERROR() \
  { REXLOG_ERROR(core, "Win32 error 0x{:08X} in {}", GetLastError(), __FUNCTION__); }

using SetThreadDescriptionFn = HANDLE (*)(HANDLE hThread, PCWSTR lpThreadDescription);

namespace rex::platform {

void EnableAffinityConfiguration() {
  HANDLE process_handle = GetCurrentProcess();
  DWORD_PTR process_affinity_mask;
  DWORD_PTR system_affinity_mask;
  GetProcessAffinityMask(process_handle, &process_affinity_mask, &system_affinity_mask);
  SetProcessAffinityMask(process_handle, system_affinity_mask);
}

u32 CurrentThreadSystemId() {
  return static_cast<u32>(GetCurrentThreadId());
}

// https://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx
#pragma pack(push, 8)
struct THREADNAME_INFO {
  DWORD dwType;     /**< Must be 0x1000. */
  LPCSTR szName;    /**< Pointer to the name in user address space. */
  DWORD dwThreadID; /**< Thread ID, -1 for the caller thread. */
  DWORD dwFlags;    /**< Reserved, must be zero. */
};
#pragma pack(pop)

void RaiseThreadNameException(HANDLE thread, const std::string& name) {
  if (!IsDebuggerPresent()) {
    return;
  }
  THREADNAME_INFO info;
  info.dwType = 0x1000;
  info.szName = name.c_str();
  info.dwThreadID = ::GetThreadId(thread);
  info.dwFlags = 0;
  __try {
    RaiseException(0x406D1388, 0, sizeof(info) / sizeof(ULONG_PTR),
                   reinterpret_cast<ULONG_PTR*>(&info));
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void SetThreadNameImpl(HANDLE thread, const std::string_view name) {
  auto kernel = GetModuleHandleW(L"kernel32.dll");
  if (kernel) {
    auto func = (SetThreadDescriptionFn)GetProcAddress(kernel, "SetThreadDescription");
    if (func) {
      int size_needed =
          MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
      std::wstring u16name(size_needed, 0);
      MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), u16name.data(),
                          size_needed);
      func(thread, u16name.c_str());
    }
  }
  RaiseThreadNameException(thread, std::string(name));
}

void SetCurrentThreadName(const std::string_view name) {
  SetThreadNameImpl(GetCurrentThread(), name);
}

void MaybeYield() {
  SwitchToThread();
  MemoryBarrier();
}

void SyncMemory() {
  MemoryBarrier();
}

namespace {

struct SleepTimer {
  SleepTimer()
      : handle(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS)) {}
  ~SleepTimer() {
    if (handle) {
      CloseHandle(handle);
    }
  }
  HANDLE handle;
};

bool WaitOnSleepTimer(std::chrono::microseconds duration) {
  thread_local SleepTimer timer;
  if (!timer.handle) {
    return false;
  }
  LARGE_INTEGER due;
  due.QuadPart = -duration.count() * 10;
  return SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE) &&
         WaitForSingleObject(timer.handle, INFINITE) == WAIT_OBJECT_0;
}

}  // namespace

void Sleep(std::chrono::microseconds duration) {
  if (duration <= std::chrono::microseconds::zero()) {
    MaybeYield();
    return;
  }
  constexpr std::chrono::microseconds kSpinTail{250};
  const auto target = std::chrono::steady_clock::now() + duration;
  if (duration > kSpinTail && !WaitOnSleepTimer(duration - kSpinTail)) {
    ::Sleep(static_cast<DWORD>(
        std::chrono::duration_cast<std::chrono::milliseconds>(duration - kSpinTail).count()));
  }
  while (std::chrono::steady_clock::now() < target) {
    YieldProcessor();
  }
}

std::chrono::milliseconds QueryCurrentThreadCPUTime() {
  FILETIME creation{};
  FILETIME exit{};
  FILETIME kernel{};
  FILETIME user{};
  if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) {
    return std::chrono::milliseconds(0);
  }
  const u64 kernel_ticks = (static_cast<u64>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
  const u64 user_ticks = (static_cast<u64>(user.dwHighDateTime) << 32) | user.dwLowDateTime;
  return std::chrono::milliseconds((kernel_ticks + user_ticks) / 10000);
}

SleepResult AlertableSleep(std::chrono::microseconds duration) {
  if (SleepEx(static_cast<DWORD>(duration.count() / 1000), TRUE) == WAIT_IO_COMPLETION) {
    return SleepResult::kAlerted;
  }
  return SleepResult::kSuccess;
}

TLSHandle AllocateTLSHandle() {
  return TlsAlloc();
}

bool FreeTLSHandle(TLSHandle handle) {
  return TlsFree(handle) ? true : false;
}

uintptr_t GetTLSValue(TLSHandle handle) {
  return reinterpret_cast<uintptr_t>(TlsGetValue(handle));
}

bool SetTLSValue(TLSHandle handle, uintptr_t value) {
  // NOLINTNEXTLINE(performance-no-int-to-ptr): TlsSetValue takes the value as a void pointer
  return TlsSetValue(handle, reinterpret_cast<void*>(value)) ? true : false;
}

template <typename T>
class Win32Handle : public T {
 public:
  explicit Win32Handle(HANDLE handle) : handle_(handle) { REX_ASSERT(handle != nullptr); }
  ~Win32Handle() override {
    CloseHandle(handle_);
    handle_ = nullptr;
  }

 protected:
  void* GetNativeHandle() const override { return handle_; }

  HANDLE handle_ = nullptr;
};

WaitResult Wait(WaitHandle* wait_handle, bool is_alertable, std::chrono::milliseconds timeout) {
  HANDLE handle = wait_handle->GetNativeHandle();
  DWORD result = WaitForSingleObjectEx(handle, DWORD(timeout.count()), is_alertable ? TRUE : FALSE);
  switch (result) {
    case WAIT_OBJECT_0:
      return WaitResult::kSuccess;
    case WAIT_ABANDONED:
      return WaitResult::kAbandoned;
    case WAIT_IO_COMPLETION:
      return WaitResult::kUserCallback;
    case WAIT_TIMEOUT:
      return WaitResult::kTimeout;
    default:
    case WAIT_FAILED:
      return WaitResult::kFailed;
  }
}

WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal, WaitHandle* wait_handle_to_wait_on,
                         bool is_alertable, std::chrono::milliseconds timeout) {
  HANDLE handle_to_signal = wait_handle_to_signal->GetNativeHandle();
  HANDLE handle_to_wait_on = wait_handle_to_wait_on->GetNativeHandle();
  DWORD result = SignalObjectAndWait(handle_to_signal, handle_to_wait_on, DWORD(timeout.count()),
                                     is_alertable ? TRUE : FALSE);
  switch (result) {
    case WAIT_OBJECT_0:
      return WaitResult::kSuccess;
    case WAIT_ABANDONED:
      return WaitResult::kAbandoned;
    case WAIT_IO_COMPLETION:
      return WaitResult::kUserCallback;
    case WAIT_TIMEOUT:
      return WaitResult::kTimeout;
    default:
    case WAIT_FAILED:
      return WaitResult::kFailed;
  }
}

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[], size_t wait_handle_count,
                                           bool wait_all, bool is_alertable,
                                           std::chrono::milliseconds timeout) {
  std::vector<HANDLE> handles(wait_handle_count);
  for (size_t i = 0; i < wait_handle_count; ++i) {
    handles[i] = wait_handles[i]->GetNativeHandle();
  }
  DWORD result =
      WaitForMultipleObjectsEx(DWORD(handles.size()), handles.data(), wait_all ? TRUE : FALSE,
                               DWORD(timeout.count()), is_alertable ? TRUE : FALSE);
  if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + handles.size()) {
    return std::pair<WaitResult, size_t>(WaitResult::kSuccess, result - WAIT_OBJECT_0);
  } else if (result >= WAIT_ABANDONED_0 && result < WAIT_ABANDONED_0 + handles.size()) {
    return std::pair<WaitResult, size_t>(WaitResult::kAbandoned, result - WAIT_ABANDONED_0);
  }
  switch (result) {
    case WAIT_IO_COMPLETION:
      return std::pair<WaitResult, size_t>(WaitResult::kUserCallback, 0);
    case WAIT_TIMEOUT:
      return std::pair<WaitResult, size_t>(WaitResult::kTimeout, 0);
    default:
    case WAIT_FAILED:
      return std::pair<WaitResult, size_t>(WaitResult::kFailed, 0);
  }
}

class Win32Event : public Win32Handle<Event> {
 public:
  explicit Win32Event(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Event() override = default;
  void Set() override { SetEvent(handle_); }
  void Reset() override { ResetEvent(handle_); }
  void Pulse() override { PulseEvent(handle_); }
};

std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
  HANDLE handle = CreateEvent(nullptr, TRUE, initial_state ? TRUE : FALSE, nullptr);
  if (handle) {
    return std::make_unique<Win32Event>(handle);
  } else {
    REX_LOG_LAST_ERROR();
    return nullptr;
  }
}

std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial_state) {
  HANDLE handle = CreateEvent(nullptr, FALSE, initial_state ? TRUE : FALSE, nullptr);
  if (handle) {
    return std::make_unique<Win32Event>(handle);
  } else {
    REX_LOG_LAST_ERROR();
    return nullptr;
  }
}

class Win32Semaphore : public Win32Handle<Semaphore> {
 public:
  explicit Win32Semaphore(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Semaphore() override = default;
  bool Release(int release_count, int* out_previous_count) override {
    return ReleaseSemaphore(handle_, release_count, reinterpret_cast<LPLONG>(out_previous_count))
               ? true
               : false;
  }
};

std::unique_ptr<Semaphore> Semaphore::Create(int initial_count, int maximum_count) {
  HANDLE handle = CreateSemaphore(nullptr, initial_count, maximum_count, nullptr);
  if (handle) {
    return std::make_unique<Win32Semaphore>(handle);
  } else {
    REX_LOG_LAST_ERROR();
    return nullptr;
  }
}

class Win32Mutant : public Win32Handle<Mutant> {
 public:
  explicit Win32Mutant(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Mutant() = default;
  bool Release() override { return ReleaseMutex(handle_) ? true : false; }
};

std::unique_ptr<Mutant> Mutant::Create(bool initial_owner) {
  HANDLE handle = CreateMutex(nullptr, initial_owner ? TRUE : FALSE, nullptr);
  if (handle) {
    return std::make_unique<Win32Mutant>(handle);
  } else {
    REX_LOG_LAST_ERROR();
    return nullptr;
  }
}

class Win32Timer : public Win32Handle<Timer> {
  using WClock = Timer::WClock;
  using GClock = Timer::GClock;

 public:
  explicit Win32Timer(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Timer() = default;

  bool SetOnceAfter(rex::HundredNanoseconds rel_time, std::function<void()> opt_callback) override {
    return SetOnceAt(WClock::now() + rel_time, std::move(opt_callback));
  }
  bool SetOnceAt(GClock::time_point due_time, std::function<void()> opt_callback) override {
    return SetOnceAt(std::chrono::clock_cast<WClock>(due_time), std::move(opt_callback));
  }
  bool SetOnceAt(WClock::time_point due_time, std::function<void()> opt_callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(opt_callback);
    LARGE_INTEGER due_time_li;
    due_time_li.QuadPart = static_cast<LONGLONG>(WClock::ToFileTime(due_time));
    auto completion_routine =
        callback_ ? reinterpret_cast<PTIMERAPCROUTINE>(CompletionRoutine) : nullptr;
    return SetWaitableTimer(handle_, &due_time_li, 0, completion_routine, this, FALSE) ? true
                                                                                       : false;
  }

  bool SetRepeatingAfter(rex::HundredNanoseconds rel_time, std::chrono::milliseconds period,
                         std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(WClock::now() + rel_time, period, std::move(opt_callback));
  }
  bool SetRepeatingAt(GClock::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(std::chrono::clock_cast<WClock>(due_time), period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(WClock::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(opt_callback);
    LARGE_INTEGER due_time_li;
    due_time_li.QuadPart = static_cast<LONGLONG>(WClock::ToFileTime(due_time));
    auto completion_routine =
        callback_ ? reinterpret_cast<PTIMERAPCROUTINE>(CompletionRoutine) : nullptr;
    return SetWaitableTimer(handle_, &due_time_li, static_cast<i32>(period.count()),
                            completion_routine, this, FALSE)
               ? true
               : false;
  }

  bool Cancel() override {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = nullptr;
    return CancelWaitableTimer(handle_) ? true : false;
  }

  bool Reset() override {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = nullptr;
    LARGE_INTEGER due_time_li;
    due_time_li.QuadPart =
        -std::chrono::duration_cast<rex::HundredNanoseconds>(std::chrono::hours(1)).count();
    return SetWaitableTimer(handle_, &due_time_li, 0, nullptr, nullptr, FALSE) &&
           CancelWaitableTimer(handle_);
  }

 private:
  static void CompletionRoutine(Win32Timer* timer, [[maybe_unused]] DWORD timer_low,
                                [[maybe_unused]] DWORD timer_high) {
    std::function<void()> callback;
    {
      std::lock_guard<std::mutex> lock(timer->mutex_);
      callback = timer->callback_;
    }
    callback();
  }

  std::mutex mutex_;
  std::function<void()> callback_;
};

std::unique_ptr<Timer> Timer::CreateManualResetTimer() {
  HANDLE handle = CreateWaitableTimer(nullptr, TRUE, nullptr);
  if (handle) {
    return std::make_unique<Win32Timer>(handle);
  } else {
    REX_LOG_LAST_ERROR();
    return nullptr;
  }
}

std::unique_ptr<Timer> Timer::CreateSynchronizationTimer() {
  HANDLE handle = CreateWaitableTimer(nullptr, FALSE, nullptr);
  if (handle) {
    return std::make_unique<Win32Timer>(handle);
  } else {
    REX_LOG_LAST_ERROR();
    return nullptr;
  }
}

extern "C" void rex_redirect_stub();

#if REX_ARCH_AMD64
__asm__(
    ".text\n"
    ".globl rex_redirect_stub\n"
    ".p2align 4\n"
    "rex_redirect_stub:\n"
    ".seh_proc rex_redirect_stub\n"
    ".seh_pushframe\n"
    "subq $32, %rsp\n"
    ".seh_stackalloc 32\n"
    ".seh_endprologue\n"
    "callq *%rax\n"
    "int3\n"
    ".seh_endproc\n");
#elif REX_ARCH_ARM64
__asm__(
    ".text\n"
    ".globl rex_redirect_stub\n"
    ".p2align 2\n"
    "rex_redirect_stub:\n"
    ".seh_proc rex_redirect_stub\n"
    ".seh_context\n"
    ".seh_endprologue\n"
    "blr x16\n"
    "brk #0xf000\n"
    ".seh_endfunclet\n"
    ".seh_endproc\n");
#endif

class Win32Thread : public Win32Handle<Thread> {
 public:
  explicit Win32Thread(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Thread() = default;

  void SetName(std::string name) override {
    SetThreadNameImpl(handle_, name);
    Thread::SetName(name);
  }

  i32 GetPriority() override { return GetThreadPriority(handle_); }
  u32 GetSystemId() const override { return GetThreadId(handle_); }

  void SetPriority(i32 new_priority) override { SetThreadPriority(handle_, new_priority); }

  u64 GetAffinityMask() override {
    DWORD_PTR prev = SetThreadAffinityMask(handle_, 1);
    if (prev) {
      SetThreadAffinityMask(handle_, prev);
    }
    return static_cast<u64>(prev);
  }

  void SetAffinityMask(u64 new_affinity_mask) override {
    SetThreadAffinityMask(handle_, new_affinity_mask);
  }

  struct APCData {
    rex::MoveOnlyFunction<void()> callback;
  };
  static void NTAPI DispatchAPC(ULONG_PTR parameter) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr): QueueUserAPC delivers its parameter as a ULONG_PTR
    std::unique_ptr<APCData> apc_data(reinterpret_cast<APCData*>(parameter));
    apc_data->callback();
  }

  void QueueUserCallback(rex::MoveOnlyFunction<void()> callback) override {
    auto apc_data = std::make_unique<ApcData>(std::move(callback));
    auto* const apc = apc_data.release();
    if (!QueueUserAPC(DispatchApc, handle_, reinterpret_cast<ULONG_PTR>(apc))) {
      apc_data.reset(apc);
      const DWORD error = GetLastError();
      if (WaitForSingleObject(handle_, 0) != WAIT_OBJECT_0) {
        SetLastError(error);
        REX_LOG_LAST_ERROR();
      }
    }
  }

  bool Resume(u32* out_previous_suspend_count = nullptr) override {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    DWORD result = ResumeThread(handle_);
    if (result == UINT_MAX) {
      return false;
    }
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = result;
    }
    return true;
  }

  bool Suspend(u32* out_previous_suspend_count = nullptr) override {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    DWORD result = SuspendThread(handle_);
    if (result == UINT_MAX) {
      return false;
    }
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = result;
    }
    return true;
  }

  bool Redirect(RedirectFilter filter, void* context, void (*target)()) override {
    if (SuspendThread(handle_) == UINT_MAX) {
      return false;
    }
    CONTEXT state{};
    state.ContextFlags = CONTEXT_FULL;
    bool redirected = false;
#if REX_ARCH_AMD64
    if (GetThreadContext(handle_, &state) && filter(state.Rip, context)) {
      const u64 frame = (state.Rsp - 5 * sizeof(u64)) & ~u64{0xF};
      const std::array<u64, 5> machine_frame{state.Rip, state.SegCs, state.EFlags, state.Rsp,
                                             state.SegSs};
      // NOLINTNEXTLINE(performance-no-int-to-ptr): CONTEXT stores the stack pointer as a DWORD64
      std::memcpy(reinterpret_cast<void*>(frame), machine_frame.data(), sizeof(machine_frame));
      state.Rsp = frame;
      state.Rip = reinterpret_cast<u64>(&rex_redirect_stub);
      state.Rax = reinterpret_cast<u64>(target);
      redirected = SetThreadContext(handle_, &state) != 0;
    }
#elif REX_ARCH_ARM64
    if (GetThreadContext(handle_, &state) && filter(state.Pc, context)) {
      const u64 frame = (state.Sp - sizeof(CONTEXT)) & ~u64{0xF};
      std::memcpy(reinterpret_cast<void*>(frame), &state, sizeof(CONTEXT));
      state.Sp = frame;
      state.Pc = reinterpret_cast<u64>(&rex_redirect_stub);
      state.X16 = reinterpret_cast<u64>(target);
      redirected = SetThreadContext(handle_, &state) != 0;
    }
#endif
    ResumeThread(handle_);
    return redirected;
  }

  bool SuspendIf(RedirectFilter filter, void* context) override {
    if (SuspendThread(handle_) == UINT_MAX) {
      return false;
    }
    CONTEXT state{};
    state.ContextFlags = CONTEXT_CONTROL;
#if REX_ARCH_AMD64
    const bool accepted = GetThreadContext(handle_, &state) && filter(state.Rip, context);
#elif REX_ARCH_ARM64
    const bool accepted = GetThreadContext(handle_, &state) && filter(state.Pc, context);
#endif
    if (!accepted) {
      ResumeThread(handle_);
    }
    return accepted;
  }

 private:
  void AssertCallingThread() { REX_ASSERT(GetCurrentThreadId() == GetThreadId(handle_)); }
};

thread_local std::unique_ptr<Win32Thread> current_thread = nullptr;

struct ThreadStartData {
  rex::MoveOnlyFunction<void()> start_routine;
};
DWORD WINAPI ThreadStartRoutine(LPVOID parameter) {
  detail::RegisterCurrentThread();
  PrepareThreadForCrash();
  current_thread = std::make_unique<Win32Thread>(::GetCurrentThread());

  std::unique_ptr<ThreadStartData> start_data(static_cast<ThreadStartData*>(parameter));
  start_data->start_routine();
  return 0;
}

std::unique_ptr<Thread> Thread::Create(CreationParameters params,
                                       rex::MoveOnlyFunction<void()> start_routine) {
  auto start_data = std::make_unique<ThreadStartData>(std::move(start_routine));
  ThreadStartData* const start = start_data.release();
  HANDLE handle = CreateThread(nullptr, params.stack_size, ThreadStartRoutine, start,
                               params.create_suspended ? CREATE_SUSPENDED : 0, nullptr);
  if (handle) {
    return std::make_unique<Win32Thread>(handle);
  }
  start_data.reset(start);
  REX_LOG_LAST_ERROR();
  return nullptr;
}

Thread* Thread::GetCurrentThread() {
  if (current_thread) {
    return current_thread.get();
  }

  HANDLE handle = ::GetCurrentThread();
  if (handle == INVALID_HANDLE_VALUE) {
    return nullptr;
  }

  current_thread = std::make_unique<Win32Thread>(handle);
  return current_thread.get();
}

void Thread::Exit(int exit_code) {
  ExitThread(exit_code);
}

}  // namespace rex::platform
