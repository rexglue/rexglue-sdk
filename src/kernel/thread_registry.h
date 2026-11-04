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
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <rex/platform/lock_order.h>
#include <rex/kernel/thread_registry.h>

#include "kernel/object_table.h"

namespace rex {
namespace kernel {
class TitleRuntime;
}  // namespace kernel
}  // namespace rex

namespace rex::kernel {

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

  /** Returns a copy of the registered thread notifications, in registration order. */
  std::vector<u32> GetThreadNotifications();

 private:
  static void Dispose(std::unique_ptr<KernelThread> thread);

  TitleRuntime& runtime_;
  ObjectTable& table_;
  rex::platform::RankedSharedMutex mutex_{rex::platform::lock_rank::kThreadRegistry};
  std::unordered_map<u32, std::unique_ptr<KernelThread>> threads_;
  std::atomic<u32> next_id_{0};
  rex::platform::RankedMutex fiber_mutex_{rex::platform::LockRank::kLeaf};
  rex::platform::RankedMutex notify_mutex_{rex::platform::LockRank::kLeaf};
  std::vector<u32> thread_notifications_;
  std::unordered_map<u32, GuestFiber> fiber_map_;
  std::mutex fiber_name_pool_mutex_;
  std::unordered_map<u32, std::unique_ptr<char[]>> fiber_name_pool_;
};

}  // namespace rex::kernel
