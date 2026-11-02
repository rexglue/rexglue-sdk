/**
 * @file        kernel/thread_registry.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <rex/platform.h>
#include <rex/platform/socket.h>
#include <rex/platform/sync.h>
#include <rex/platform/thread.h>
#include <rex/ppc/context.h>
#include <rex/result.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/kernel/types/status.h>
#include <rex/types.h>
#include <rex/module.h>
#include <rex/utility.h>

namespace rex::kernel {

/** A transient pin on a `KernelThread`. Never stored. Move only. */
class ThreadPin : public rex::MoveOnly {
 public:
  ThreadPin() = default;
  /** Takes a pin already taken on a `KernelThread`. */
  explicit ThreadPin(KernelThread* thread) noexcept : thread_(thread) {}
  ThreadPin(ThreadPin&& other) noexcept : thread_(other.thread_) { other.thread_ = nullptr; }
  ThreadPin& operator=(ThreadPin&& other) noexcept {
    if (this != &other) {
      Reset();
      thread_ = other.thread_;
      other.thread_ = nullptr;
    }
    return *this;
  }
  ~ThreadPin() { Reset(); }

  explicit operator bool() const noexcept { return thread_ != nullptr; }
  KernelThread* operator->() const noexcept { return thread_; }
  KernelThread& operator*() const noexcept { return *thread_; }
  [[nodiscard]] KernelThread* GetThread() const noexcept { return thread_; }

  void Reset() noexcept {
    if (thread_ != nullptr) {
      if (thread_->pins.Unpin()) {
        delete thread_;
      }
      thread_ = nullptr;
    }
  }

 private:
  KernelThread* thread_ = nullptr;
};

/** Host data of a shim fiber. */
struct GuestFiber {
  rex::platform::Fiber* host_fiber;
  u32 uid; /**< Unique ID for profiler names. */
  u32 guest_context_addr;
  u32 guest_stack_bottom; /**< First byte of the guest stack allocation, 0 for a thread fiber. */
  bool is_thread_fiber;   /**< True for ConvertThreadToFiber, whose stack the thread owns. */
};

/** Waits on host and guest objects for XAM. A wait on a guest thread throws ThreadStop when the
 * thread must stop. */
class IObjectWaiter {
 public:
  virtual ~IObjectWaiter() = default;

  /**
   * On a guest thread a stop request throws ThreadStop, and any other wake resumes the wait for the
   * time left.
   *
   * @param timeout The longest wait, milliseconds::max() for no limit.
   */
  virtual rex::platform::WaitResult WaitHostObject(rex::platform::WaitHandle* handle,
                                                   std::chrono::milliseconds timeout) = 0;

  /**
   * Waits on the object a guest handle names, succeeding at once for a type without a wait handle.
   *
   * @param handle The handle, pseudo handles are translated.
   * @param alertable Nonzero when a user APC ends the wait.
   * @param opt_timeout The timeout in 100 nanosecond ticks, null to wait forever.
   * @return X_STATUS_SUCCESS, X_STATUS_USER_APC, X_STATUS_TIMEOUT, X_STATUS_ABANDONED_WAIT_0, or
   * X_STATUS_INVALID_HANDLE.
   */
  virtual X_STATUS WaitGuestObject(X_HANDLE handle, u32 wait_reason, u32 processor_mode,
                                   u32 alertable, u64* opt_timeout) = 0;

  /**
   * Waits on the objects several guest handles name, as NtWaitForMultipleObjectsEx does. Delivers
   * the pending user APCs when an alertable wait ends with X_STATUS_USER_APC.
   *
   * @param handles The handles, pseudo handles are translated.
   * @param wait_type 0 to wait for any object, 1 to wait for all of them.
   * @param alertable Nonzero when a user APC ends the wait.
   * @param opt_timeout The timeout in 100 nanosecond ticks, null to wait forever.
   * @return The status of the wait, or X_STATUS_INVALID_PARAMETER when a handle names no object.
   */
  virtual X_STATUS WaitGuestObjects(std::span<const X_HANDLE> handles, u32 wait_type,
                                    u32 wait_reason, u32 processor_mode, u32 alertable,
                                    u64* opt_timeout) = 0;

  /**
   * Waits until a socket may be ready, with the stop behavior of WaitHostObject.
   *
   * @param timeout The longest wait, milliseconds::max() for no limit.
   * @return kSuccess when a socket may be ready, kTimeout or kFailed.
   */
  virtual rex::platform::WaitResult WaitHostSockets(
      std::span<const rex::platform::SocketWait> sockets, std::chrono::milliseconds timeout) = 0;
};

/** Owns every `KernelThread`, finds them by ID or handle, and keeps the TLS slots and fibers. */
REX_DECLARE_INTERFACE(IThreadRegistry, 1)
  virtual ~IThreadRegistry() = default;

  /**
   * Takes ownership of a `KernelThread` and starts it with a handle count of 1.
   *
   * @return The creation handle, or the status of the allocation or the thread creation.
   */
  [[nodiscard]] virtual Result<X_HANDLE> Launch(std::unique_ptr<KernelThread> thread) = 0;

  /**
   * Starts a guest thread that runs a title task, with a 0 stack size, no trampoline and no flags.
   *
   * @param start_address The guest address of the task entry point.
   * @param start_context The argument passed to the entry point.
   * @return The creation handle, or the status of the allocation or the thread creation.
   */
  [[nodiscard]] virtual Result<X_HANDLE> CreateTaskThread(u32 start_address, u32 start_context) = 0;

  /** Returns the thread the caller runs on, which the registry owns. */
  [[nodiscard]] virtual ICallingThread* GetCallingThread() = 0;

  [[nodiscard]] virtual IObjectWaiter& GetObjectWaiter() = 0;

  /** Sleeps the calling thread. A kernel thread with a stop request wakes early. */
  virtual void SleepCurrentThread(std::chrono::microseconds duration) = 0;

  /**
   * Finds a thread by ID and pins it.
   *
   * @return The pin, empty when no `KernelThread` has the ID.
   */
  [[nodiscard]] virtual ThreadPin Find(u32 thread_id) = 0;

  /**
   * Finds a thread by handle and pins it.
   *
   * @param handle The handle, pseudo handles are translated.
   * @return The pin, empty when the handle is invalid or names another type.
   */
  [[nodiscard]] virtual ThreadPin FindByHandle(X_HANDLE handle) = 0;

  [[nodiscard]] virtual std::vector<ThreadPin> All() = 0;

  /**
   * Claims a TLS slot in the process of the calling guest thread under the guest TLS spinlock.
   *
   * @return The slot, empty when the process has no free slot or the caller is no guest thread.
   */
  [[nodiscard]] virtual std::optional<u32> AllocateTLS(PPCContext* context) = 0;

  /** Releases a TLS slot and clears it in every thread of the calling thread's process. */
  virtual void FreeTLS(PPCContext* context, u32 slot) = 0;

  /**
   * Finds a fiber by its guest context block.
   *
   * @param address The guest address of the fiber context block.
   * @return A copy of the fiber data, empty when no fiber has the address.
   */
  [[nodiscard]] virtual std::optional<GuestFiber> FindFiber(u32 address) = 0;

  /**
   * Registers a fiber, replacing the fiber with the same address.
   *
   * @param address The guest address of the fiber context block.
   */
  virtual void RegisterFiber(u32 address, const GuestFiber& info) = 0;

  virtual void UnregisterFiber(u32 address) = 0;

  /**
   * Returns a fiber name for the profiler, never freed.
   *
   * @param fiber_uid The fiber UID the name is keyed by.
   * @param thread_name The name of the thread that owns the fiber.
   */
  [[nodiscard]] virtual const char* GetOrCreateFiberName(u32 fiber_uid,
                                                         const char* thread_name) = 0;

  /**
   * Registers a thread notification.
   *
   * @param notification Address of the title's XTHREAD_NOTIFICATION.
   */
  virtual void RegisterThreadNotify(u32 notification) = 0;

  /**
   * Removes a thread notification.
   *
   * @param notification Address of the title's XTHREAD_NOTIFICATION.
   */
  virtual void UnregisterThreadNotify(u32 notification) = 0;
};

}  // namespace rex::kernel

namespace rex::kernel {

/**
 * The values an overlapped completion writes. An empty extended error is the result, and an empty
 * length is 0 on success and 0xFFFFFFFF on failure.
 */
struct CompletionOptions {
  X_RESULT result;
  std::optional<u32> extended_error =
      std::nullopt;                         /**< The extended error, empty for the result. */
  std::optional<u32> length = std::nullopt; /**< The length, empty for the default of the result. */
};

/** Completes overlapped operations. */
REX_DECLARE_INTERFACE(IOverlappedDispatcher, 1)
  virtual ~IOverlappedDispatcher() = default;

  /**
   * Completes an overlapped operation on the calling thread.
   *
   * @param overlapped The guest address of the XOVERLAPPED.
   * @param options The result, extended error and length to write.
   */
  virtual void Complete(u32 overlapped, const CompletionOptions& options) = 0;

  /**
   * Runs the writes that complete an overlapped block, result first and then event.
   * Observe sees both writes or neither.
   *
   * @param publish Stores the result and sets the event.
   */
  virtual void Publish(rex::MoveOnlyFunction<void()> publish) = 0;

  /**
   * Runs the reads that observe an overlapped block. A result seen as complete has its event set.
   *
   * @param observe Reads the result.
   */
  virtual u32 Observe(rex::MoveOnlyFunction<u32()> observe) = 0;

  /**
   * Marks an overlapped operation pending and completes it on the dispatch thread after the work.
   *
   * @param overlapped The guest address of the XOVERLAPPED.
   * @param work The function that does the operation.
   * @param options The result, extended error and length to write.
   * @param pre Runs before the work, may be empty.
   * @param post Runs after the completion, may be empty.
   */
  virtual void CompleteDeferred(u32 overlapped, rex::MoveOnlyFunction<void()> work,
                                const CompletionOptions& options,
                                rex::MoveOnlyFunction<void()> pre = {},
                                rex::MoveOnlyFunction<void()> post = {}) = 0;

  /**
   * Like the overload above, but the work returns the options.
   *
   * @param overlapped The guest address of the XOVERLAPPED.
   * @param work The work, returning the result, extended error and length to write.
   * @param pre Runs before the work, may be empty.
   * @param post Runs after the completion, may be empty.
   */
  virtual void CompleteDeferred(u32 overlapped, rex::MoveOnlyFunction<CompletionOptions()> work,
                                rex::MoveOnlyFunction<void()> pre = {},
                                rex::MoveOnlyFunction<void()> post = {}) = 0;
};

/** Holds the DPC list. */
REX_DECLARE_INTERFACE(IDPCQueue, 1)
  virtual ~IDPCQueue() = default;

  /**
   * Queues a DPC unless it is queued. The caller drains when its IRQL allows it.
   *
   * @param dpc Address of the title's KDPC.
   * @param arg1 Passed to the routine as SystemArgument1.
   * @param arg2 Passed to the routine as SystemArgument2.
   * @return True when the DPC was newly queued, false when it was in the queue.
   */
  virtual bool Queue(u32 dpc, u32 arg1, u32 arg2) = 0;

  /**
   * Removes a queued DPC.
   *
   * @param dpc Address of the title's KDPC.
   * @return True when the DPC was queued.
   */
  virtual bool Remove(u32 dpc) = 0;

  /**
   * Runs every queued DPC one at a time on the DPC stack at DISPATCH_LEVEL. Does nothing inside an
   * ISR or DPC, on a thread with no title state, or while another thread drains.
   */
  virtual void Drain() = 0;
};

}  // namespace rex::kernel
