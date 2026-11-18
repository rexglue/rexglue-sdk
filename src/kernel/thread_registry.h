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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <rex/assert.h>
#include <rex/platform/lock_order.h>
#include <rex/kernel/thread_registry.h>

#include "kernel/object_table.h"

namespace rex {
namespace kernel {
class TitleRuntime;
}  // namespace kernel
}  // namespace rex

namespace rex::kernel {

/**
 * Counts the host threads that run a KernelThread from creation to the end of their entry
 * function, which includes the deletion of the KernelThread. The host threads share ownership, so
 * the fence outlives the registry.
 */
class HostFence {
 public:
  void Enter() {
    std::lock_guard lock(mutex_);
    ++count_;
  }

  void Leave() {
    {
      std::lock_guard lock(mutex_);
      REX_ASSERT(count_ > 0);
      --count_;
    }
    idle_.notify_all();
  }

  /** Returns the number of host threads that run. */
  [[nodiscard]] u32 GetCount() {
    std::lock_guard lock(mutex_);
    return count_;
  }

  /**
   * Waits until no more than the given number of host threads run. It never gives up, because a
   * host thread that still runs would use what the caller frees next.
   *
   * @tparam Report A callable taking the number of host threads that run.
   * @param remaining The host threads allowed to run, 1 when the caller is one of them.
   * @param step The time between two calls of report.
   * @param report Called after each step in which the host threads did not end.
   */
  template <typename Report>
  void WaitIdle(u32 remaining, std::chrono::milliseconds step, Report report) {
    for (;;) {
      u32 running = 0;
      {
        std::unique_lock lock(mutex_);
        if (idle_.wait_for(lock, step, [&] { return count_ <= remaining; })) {
          return;
        }
        running = count_;
      }
      report(running);
    }
  }

 private:
  std::mutex mutex_;
  std::condition_variable idle_;
  u32 count_ = 0;
};

/**
 * The place of one KernelThread in the HostFence. The KernelThread and its host thread share it,
 * and the first of them to call Leave removes the thread from the count, so the count leaves once
 * whichever of the two ends the thread.
 */
class HostSlot {
 public:
  explicit HostSlot(std::shared_ptr<HostFence> fence) : fence_(std::move(fence)) {
    fence_->Enter();
  }

  /** Called by the host thread when its entry function starts. */
  void MarkEntered() { entered_.store(true, std::memory_order_release); }

  [[nodiscard]] bool IsEntered() const { return entered_.load(std::memory_order_acquire); }

  void Leave() {
    if (!left_.exchange(true, std::memory_order_acq_rel)) {
      fence_->Leave();
    }
  }

 private:
  std::shared_ptr<HostFence> fence_;
  std::atomic<bool> entered_{false};
  std::atomic<bool> left_{false};
};

class ThreadRegistry final : public IThreadRegistry, public IObjectWaiter {
 public:
  /**
   * @param runtime The runtime whose process blocks hold the TLS bitmaps.
   * @param table Table of thread handles.
   */
  ThreadRegistry(TitleRuntime& runtime, ObjectTable& table);
  ~ThreadRegistry() override;

  [[nodiscard]] Result<X_HANDLE> Launch(std::unique_ptr<KernelThread> thread) override;
  [[nodiscard]] Result<X_HANDLE> CreateTaskThread(u32 start_address, u32 start_context) override;
  [[nodiscard]] ICallingThread* GetCallingThread() override;
  [[nodiscard]] IObjectWaiter& GetObjectWaiter() override { return *this; }
  rex::platform::WaitResult WaitHostObject(rex::platform::WaitHandle* handle,
                                           std::chrono::milliseconds timeout) override;
  X_STATUS WaitGuestObject(X_HANDLE handle, u32 wait_reason, u32 processor_mode, u32 alertable,
                           u64* opt_timeout) override;
  X_STATUS WaitGuestObjects(std::span<const X_HANDLE> handles, u32 wait_type, u32 wait_reason,
                            u32 processor_mode, u32 alertable, u64* opt_timeout) override;
  rex::platform::WaitResult WaitHostSockets(std::span<const rex::platform::SocketWait> sockets,
                                            std::chrono::milliseconds timeout) override;
  void SleepCurrentThread(std::chrono::microseconds duration) override;
  ThreadPin Find(u32 thread_id) override;
  ThreadPin FindByHandle(X_HANDLE handle) override;
  std::vector<ThreadPin> All() override;
  std::optional<u32> AllocateTLS(PPCContext* context) override;
  void FreeTLS(PPCContext* context, u32 slot) override;
  std::optional<GuestFiber> FindFiber(u32 address) override;
  void RegisterFiber(u32 address, const GuestFiber& info) override;
  void UnregisterFiber(u32 address) override;
  const char* GetOrCreateFiberName(u32 address, const char* thread_name) override;
  void RegisterThreadNotify(u32 notification) override;
  void UnregisterThreadNotify(u32 notification) override;

  /**
   * Marks a thread as finished. The KernelThread is destroyed once its handles are closed and no
   * pin is held.
   */
  void MarkExited(KernelThread& thread);

  /**
   * Marks the last handle of a thread as closed. The KernelThread is destroyed once the thread
   * finished and no pin is held.
   */
  void MarkHandlesClosed(u32 thread_id);

  /** Destroys the host fibers no title deleted and frees their guest stacks and context blocks. */
  void DestroyFibers();

  /**
   * Waits until every host thread of a KernelThread has ended its entry function. Teardown calls it
   * before it frees what a KernelThread reads while it is destroyed, which the destruction of a
   * thread that left the registry does after the wait on the host thread handles is over.
   */
  void WaitForHosts(std::chrono::milliseconds interval);

  /** Returns the fence that counts the host threads, shared with each of them. */
  [[nodiscard]] std::shared_ptr<HostFence> GetHostFence() const { return host_fence_; }

  /** Returns a copy of the registered thread notifications, in registration order. */
  std::vector<u32> GetThreadNotifications();

 private:
  static void Dispose(std::unique_ptr<KernelThread> thread);

  TitleRuntime& runtime_;
  ObjectTable& table_;
  rex::platform::RankedSharedMutex mutex_{rex::platform::lock_rank::kThreadRegistry};
  std::unordered_map<u32, std::unique_ptr<KernelThread>> threads_;
  std::atomic<u32> next_id_{0};
  std::shared_ptr<HostFence> host_fence_ = std::make_shared<HostFence>();
  rex::platform::RankedMutex fiber_mutex_{rex::platform::LockRank::kLeaf};
  rex::platform::RankedMutex notify_mutex_{rex::platform::LockRank::kLeaf};
  std::vector<u32> thread_notifications_;
  std::unordered_map<u32, GuestFiber> fiber_map_;
  std::mutex fiber_name_pool_mutex_;
  std::unordered_map<u32, std::unique_ptr<char[]>> fiber_name_pool_;
};

}  // namespace rex::kernel
