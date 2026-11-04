/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        kernel/object/kernel_thread.h
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/assert.h>
#include <rex/kernel/types/status.h>
#include <rex/platform.h>
#include <rex/kernel/object/object.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/types/list.h>
#include <rex/kernel/types/memory.h>
#include <rex/kernel/types/thread.h>
#include <rex/platform/thread.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex {
namespace kernel {
class TitleRuntime;
}  // namespace kernel
}  // namespace rex

namespace rex::kernel {

class ThreadRegistry;

/** Table slot data that names the `KernelThread` of a thread ID. */
struct ThreadRef : public rex::NonCopyable {
  ThreadRef(u32 thread_id_in, ThreadRegistry* registry_in)
      : thread_id(thread_id_in), registry(registry_in) {}
  /** Notifies the registry that the thread lost its handles. */
  REX_KERNEL_API ~ThreadRef();

  u32 thread_id;            /**< ID of the thread whose `KernelThread` the registry holds. */
  ThreadRegistry* registry; /**< Registry notified when the thread loses its handles. */
};

/** What XAM reads and writes on the calling guest thread. */
class ICallingThread {
 public:
  virtual ~ICallingThread() = default;

  virtual u32 GetLastError() = 0;

  virtual void SetLastError(u32 error_code) = 0;

  /** Reports whether this is the entry point thread. */
  virtual bool IsMainThread() const = 0;
};

/** A guest thread or a host worker, owned by the thread registry. */
class REX_KERNEL_API KernelThread : public ICallingThread, public rex::NonCopyable {
 public:
  /** Lowest guest address AllocateGuestStack places a stack at. */
  static constexpr u32 kStackAddressRangeBegin = 0x70000000;
  /** Highest guest address AllocateGuestStack places a stack at, exclusive end of the range. */
  static constexpr u32 kStackAddressRangeEnd = 0x7F000000;

  struct CreationParams {
    u32 stack_size;          /**< Guest stack size in bytes, 0 for the process default. */
    u32 xapi_thread_startup; /**< Guest address of the startup trampoline, 0 for a raw thread. */
    u32 start_address;       /**< Guest address of the entry point. */
    u32 start_context;       /**< Argument passed to the entry point. */
    u32 creation_flags;      /**< Creation flag bits, bit 0 is `kXCreateSuspended`. */
    u32 title_process;       /**< Guest address of the process block, 0 for the title process. */
  };

  /**
   * Builds a guest `KernelThread`. The host thread starts when the registry launches the
   * `KernelThread`.
   *
   * @param stack_size The guest stack size, 0 for the process default.
   * @param xapi_thread_startup The startup trampoline, or 0 for a raw thread.
   * @param creation_flags The creation flags, bit 0 creates the thread suspended.
   * @param guest_thread True for a thread created by the guest.
   * @param main_thread True for the entry point thread.
   * @param title_process The title process block address, 0 for the title process.
   */
  KernelThread(TitleRuntime& runtime, u32 stack_size, u32 xapi_thread_startup, u32 start_address,
               u32 start_context, u32 creation_flags, bool guest_thread, bool main_thread = false,
               u32 title_process = 0);

  /**
   * Builds a host worker `KernelThread`.
   *
   * @param host_fn The function the worker runs. Its result is the exit code.
   */
  KernelThread(TitleRuntime& runtime, u32 stack_size, u32 creation_flags,
               rex::MoveOnlyFunction<int()> host_fn);
  /** Frees the host thread, the scratch block, the stack, the PCR and the kernel thread block. */
  ~KernelThread();

  static bool IsInThread(const KernelThread* other);

  static bool IsInThread();

  static KernelThread* GetCallingThread();

  static u32 GetCurrentThreadHandle();

  /**
   * Throws rex::ThreadStop when the calling thread has a stop request or the title is terminating.
   */
  static void CheckStop();

  /** Sleeps the calling thread, waking early to stop when it has a stop request. */
  static void SleepFor(std::chrono::microseconds duration);

  /**
   * Writes the CPU time of the calling thread to its KTHREAD, if it has a `KernelThread`.
   */
  static void WriteKernelTime();

  TitleRuntime& GetRuntime() const { return *runtime_; }

  kernel::GuestAddressSpace* GetAddressSpace() const;

  /**
   * @return The handle, 0 before launch.
   */
  X_HANDLE GetHandle() const { return handle_; }

  u32 GetGuestObject() const { return guest_object_ptr_; }

  /** Returns the thread block as a host pointer. */
  template <typename T>
  T* GetGuestObject() {
    return GetAddressSpace()->TranslateVirtual<T*>(guest_object_ptr_);
  }

  const std::string& GetName() const { return thread_name_; }

  /**
   * Returns the thread name without allocating or locking, for a crash report, cut to 63
   * characters.
   */
  std::string_view GetCrashName() const {
    return {crash_name_.data(), crash_name_length_.load(std::memory_order_acquire)};
  }

  const CreationParams* GetCreationParams() const { return &creation_params_; }

  u32 GetTLSPtr() const { return tls_static_address_; }

  u32 GetPCRPtr() const { return pcr_address_; }

  bool IsGuestThread() const { return guest_thread_; }

  bool IsMainThread() const override { return main_thread_; }

  bool IsRunning() const { return running_; }

  u32 GetThreadID() const { return thread_id_; }

  u32 GetLastError() override;

  void SetLastError(u32 error_code) override;

  /**
   * @param name The name. The handle is appended to it.
   */
  void SetName(const std::string_view name);

  /**
   * Exits the calling thread by unwinding to its entry.
   */
  [[noreturn]] void Exit(int exit_code);

  /** Asks the thread to stop at its next wait or kernel call and wakes it. */
  void RequestStop(i32 exit_code = 0);

  /**
   * Unwinds the thread to its entry when it runs recompiled code.
   *
   * @return True when the thread now unwinds to its entry.
   */
  bool ForceStop();

  rex::platform::Fiber* GetMainFiber() const { return main_fiber_; }

  void SetMainFiber(rex::platform::Fiber* fiber) { main_fiber_ = fiber; }

  /** Disables APC delivery by 1 level. */
  void EnterCriticalRegion();

  /** Enables APC delivery by 1 level. */
  void LeaveCriticalRegion();

  void DeliverAPCs();

  /**
   * Reports whether a user APC is queued and APC delivery is enabled.
   *
   * @return True when the next alertable wait delivers an APC.
   */
  bool HasPendingAPCs();

  void EnqueueAPC(u32 normal_routine, u32 normal_context, u32 arg1, u32 arg2);

  i32 GetPriority() const { return priority_; }

  i32 GetHostPriority();

  void SetPriority(i32 increment);

  /**
   * Sets the affinity. Logical processors are 0 core 0 thread 0, 1 core 0 thread 1, 2 core 1
   * thread 0, 3 core 1 thread 1, 4 core 2 thread 0 and 5 core 2 thread 1.
   *
   * @param affinity Processor mask. The highest set bit selects the processor.
   * @return False and no change when the mask is 0 or names a processor past the sixth.
   */
  bool SetAffinity(u32 affinity);

  u8 GetActiveCPU() const;

  void SetActiveCPU(u8 cpu_index);

  /**
   * @param value_out Receives the value.
   * @return True when the slot is in range.
   */
  bool GetTLSValue(u32 slot, u32* value_out) const;

  /**
   * @return True when the slot is in range.
   */
  bool SetTLSValue(u32 slot, u32 value);

  u32 GetSuspendCount();

  /**
   * @param out_suspend_count Receives the previous count, may be null.
   * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
   */
  X_STATUS Resume(u32* out_suspend_count = nullptr);

  /**
   * @param out_suspend_count Receives the previous count, may be null.
   * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
   */
  X_STATUS Suspend(u32* out_suspend_count = nullptr);

  /**
   * Sleeps the calling thread.
   *
   * @param alertable True when the sleep ends on a user APC.
   * @param interval The interval in 100 nanosecond ticks, negative for relative.
   * @return X_STATUS_SUCCESS, or X_STATUS_USER_APC.
   */
  X_STATUS Delay(u32 processor_mode, u32 alertable, u64 interval) const;

  rex::platform::Thread* GetThread() { return thread_.get(); }

  kernel::ThreadState* GetThreadState() { return thread_state_.get(); }

  PinCount pins; /**< Transient pins that keep the object alive while a caller uses it. */

 protected:
  void FreeStack();
  void InitializeGuestObject();

  void RundownAPCs();
  void CallThreadNotifyRoutines(bool create);

  CreationParams creation_params_ = {0, 0, 0, 0, 0, 0};

  u32 thread_id_ = 0;
  /** Guest address of the scratch block that carries the arguments of APC kernel routine calls. */
  u32 scratch_address_ = 0;
  u32 scratch_size_ = 0; /**< Size in bytes of the scratch block, 64. */
  u32 tls_static_address_ = 0;
  u32 tls_dynamic_address_ = 0;
  /** Number of 4 byte dynamic TLS slots, read by the TLS accessors for bounds checks. */
  std::atomic<u32> tls_slot_count_{0};
  u32 pcr_address_ = 0;
  u32 stack_alloc_base_ = 0;
  /** Highest stack address, where the TLS block starts, stored in the PCR and the X_KTHREAD. */
  u32 stack_base_ = 0;
  u32 stack_limit_ = 0;
  bool guest_thread_ = false;
  bool main_thread_ = false;
  std::atomic<bool> running_{false};
  /** True once the thread began exiting, which makes CheckStop return without throwing. */
  std::atomic<bool> exiting_{false};
  /** Exit code carried by the ThreadStop exception that ends the thread. */
  std::atomic<i32> stop_exit_code_{0};
  /** Guards stop_requested_, thread_ and thread_state_ against a concurrent stop request. */
  std::mutex stop_mutex_;
  /** True once a stop was requested, applied to the thread state when it is created. */
  bool stop_requested_ = false;

  std::string thread_name_;
  /** Copy of the name cut to 63 characters, readable by a crash report without locking. */
  std::array<char, 64> crash_name_{};
  std::atomic<u8> crash_name_length_{0};
  std::unique_ptr<kernel::ThreadState> thread_state_;

  i32 priority_ = 0;

  struct SuspendWait;
  /**
   * Holds the mutex that guards the suspend count in the X_KTHREAD and suspend_deferred_, and the
   * condition variable Resume signals to release a thread waiting in a deferred suspend.
   */
  std::unique_ptr<SuspendWait> suspend_wait_;
  /** True when Suspend could not stop the thread, which suspends itself at its next CheckStop. */
  std::atomic<bool> suspend_deferred_{false};

  std::mutex thread_lock_; /**< Guards thread_name_, crash_name_ and the host thread name. */

  std::unique_ptr<rex::platform::Thread> thread_ = nullptr;
  rex::platform::Fiber* main_fiber_ = nullptr;

 private:
  friend class ThreadRegistry;

  /**
   * Writes the guest object and creates the host thread without running it.
   *
   * @return X_STATUS_SUCCESS, or the failure status.
   */
  X_STATUS Create();

  /** Runs the host thread unless the title created the thread suspended. */
  void Start();

  /** Ends a created host thread that never ran, and waits until it has ended. */
  void Abandon();
  void SetHandle(X_HANDLE handle) { handle_ = handle; }
  void SetGuestObject(u32 address) { guest_object_ptr_ = address; }
  int Execute();
  void Finish(int exit_code);
  [[noreturn]] static void ThrowStop();
  void ReleaseSelf();

  /** Removes the pointer reference the thread holds on its own object. */
  void ReleaseReference();

  /**
   * Increments the suspend count and blocks until another thread resumes the caller.
   *
   * @return The previous count.
   */
  u32 SelfSuspend();

  /** Blocks the calling thread until a resume clears its deferred suspend. */
  void TakeDeferredSuspend();

  /**
   * Returns the guest address of the process block the thread belongs to, 0 before the runtime has
   * kernel globals.
   */
  u32 GetProcessAddress() const;

  void WriteExitState(u32 exit_code);

  TitleRuntime* runtime_;
  ThreadRegistry* registry_ = nullptr;
  X_HANDLE handle_ = 0;
  u32 guest_object_ptr_ = 0;
  rex::MoveOnlyFunction<int()> host_fn_;
  std::atomic<bool> released_{false};
  std::atomic<bool> abandoned_{false};
  bool exited_ = false;
  bool handles_closed_ = false;
};

}  // namespace rex::kernel
