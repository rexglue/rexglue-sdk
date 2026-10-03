/**
 * @file        runtime/thread_registry.h
 * @brief       The thread registry of a runtime
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
#include <rex/runtime/thread_registry.h>

#include "runtime/object_table.h"

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

/** The thread registry of a runtime. */
class ThreadRegistry final : public IThreadRegistry {
 public:
  /**
   * @param runtime The runtime whose process blocks hold the TLS bitmaps.
   * @param table The table that holds the thread handles.
   */
  ThreadRegistry(Runtime& runtime, ObjectTable& table);
  ~ThreadRegistry() override;

  Result<X_HANDLE> Launch(std::unique_ptr<ThreadRecord> record) override;
  ThreadPin Find(u32 thread_id) override;
  ThreadPin FindByHandle(X_HANDLE handle) override;
  std::vector<ThreadPin> All() override;
  std::optional<u32> AllocateTLS(PPCContext* context) override;
  void FreeTLS(PPCContext* context, u32 slot) override;
  std::optional<FiberInfo> LookupFiber(u32 address) override;
  void RegisterFiber(u32 address, const FiberInfo& info) override;
  void UnregisterFiber(u32 address) override;
  const char* GetOrCreateFiberName(u32 address, const char* thread_name) override;
  void RegisterThreadNotify(u32 notification) override;
  void RemoveThreadNotify(u32 notification) override;

  /**
   * Records that a thread finished running. The record is destroyed when its last handle is
   * already closed and no pin is held.
   *
   * @param record The record of the finished thread.
   */
  void NoteExited(ThreadRecord& record);

  /**
   * Records that the last handle of a thread closed. The record is destroyed when the thread
   * already finished and no pin is held.
   *
   * @param thread_id The thread id.
   */
  void NoteHandlesClosed(u32 thread_id);

  /** Counts a thread whose host thread began running its record. */
  void NoteStarted();

  /** Uncounts a started thread once its exit no longer touches the runtime. */
  void NoteStopped();

  /**
   * Waits until every started thread has stopped touching the runtime.
   *
   * @param timeout The longest wait.
   * @return True when every started thread stopped in time.
   */
  bool JoinThreads(std::chrono::milliseconds timeout);

  /** Destroys the host fibers no title deleted and frees their guest stacks and context blocks. */
  void DestroyFibers();

  /**
   * Returns a copy of the registered thread notifications.
   *
   * @return The addresses of the title's XTHREAD_NOTIFICATION blocks, in registration order.
   */
  std::vector<u32> ThreadNotifications();

 private:
  static void Dispose(std::unique_ptr<ThreadRecord> record);

  Runtime& runtime_;
  ObjectTable& table_;
  rex::platform::RankedSharedMutex mutex_{rex::platform::LockRank::ThreadRegistry};
  std::unordered_map<u32, std::unique_ptr<ThreadRecord>> threads_;
  std::atomic<u32> next_id_{0};
  std::atomic<u32> started_threads_{0};
  rex::platform::RankedMutex fiber_mutex_{rex::platform::LockRank::Leaf};
  rex::platform::RankedMutex notify_mutex_{rex::platform::LockRank::Leaf};
  std::vector<u32> thread_notifications_;
  std::unordered_map<u32, FiberInfo> fiber_map_;
  std::mutex fiber_name_pool_mutex_;
  std::unordered_map<u32, std::unique_ptr<char[]>> fiber_name_pool_;
};

}  // namespace rex::runtime
