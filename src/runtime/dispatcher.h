/**
 * @file        runtime/dispatcher.h
 * @brief       The dispatcher of a runtime and its dispatch thread
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
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <vector>

#include <rex/macros.h>
#include <rex/platform/lock_order.h>
#include <rex/platform/timer_queue.h>
#include <rex/runtime/dispatcher.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

struct X_KPCR;
class ThreadState;

/** Marks the calling thread as running an ISR. */
REX_RUNTIME_API void EnterInterrupt();

/**
 * Marks the end of an ISR on the calling thread.
 *
 * @return True when the outermost ISR ended.
 */
REX_RUNTIME_API bool LeaveInterrupt();

/** The dispatcher of a runtime, its DPC queue and its dispatch thread. */
class REX_RUNTIME_API Dispatcher final : public IDispatcher {
 public:
  /**
   * @param runtime The runtime whose memory, table and thread registry completions use.
   */
  explicit Dispatcher(Runtime& runtime);
  ~Dispatcher() override;

  void CompleteOverlappedImmediate(u32 overlapped_ptr, X_RESULT result) override;
  void CompleteOverlappedImmediateEx(u32 overlapped_ptr, X_RESULT result, u32 extended_error,
                                     u32 length) override;
  void CompleteOverlappedDeferred(std::function<void()> completion_callback, u32 overlapped_ptr,
                                  X_RESULT result, std::function<void()> pre_callback = nullptr,
                                  std::function<void()> post_callback = nullptr) override;
  void CompleteOverlappedDeferredEx(std::function<void()> completion_callback, u32 overlapped_ptr,
                                    X_RESULT result, u32 extended_error, u32 length,
                                    std::function<void()> pre_callback = nullptr,
                                    std::function<void()> post_callback = nullptr) override;
  void CompleteOverlappedDeferred(std::function<X_RESULT()> completion_callback, u32 overlapped_ptr,
                                  std::function<void()> pre_callback = nullptr,
                                  std::function<void()> post_callback = nullptr) override;
  void CompleteOverlappedDeferredEx(std::function<X_RESULT(u32&, u32&)> completion_callback,
                                    u32 overlapped_ptr,
                                    std::function<void()> pre_callback = nullptr,
                                    std::function<void()> post_callback = nullptr) override;
  bool QueueDPC(u32 dpc, u32 arg1, u32 arg2) override;
  bool RemoveDPC(u32 dpc) override;
  void DrainDPCs() override;

  /**
   * Reports whether the DPC queue holds an entry.
   *
   * @return True when a DPC is queued.
   */
  bool HasQueuedDPCs();

  /**
   * Runs work on the dispatch thread.
   *
   * @param work The function the dispatch thread runs.
   */
  void Post(std::function<void()> work);

  /** Drains the DPC queue on the dispatch thread, standing in for the clock interrupt. */
  void PostDrain();

  /**
   * Reports whether the caller is the dispatch thread.
   *
   * @return True on the dispatch thread.
   */
  bool IsDispatchThread() const;

  /**
   * Reports whether the dispatch thread runs.
   *
   * @return True between Start and Stop.
   */
  bool IsRunning() const;

  /**
   * Registers a KTIMER expiry that Stop disarms.
   *
   * @param expiry The queued expiry of a KTIMER.
   * @return False when the dispatcher is stopped, and the caller disarms the expiry.
   */
  bool TrackTimerExpiry(std::weak_ptr<rex::platform::TimerQueueWaitItem> expiry);

  /** Allocates the DPC stack and starts the dispatch thread unless it runs. */
  void Start();

  /**
   * Disarms the KTIMER expiries, stops the dispatch thread, terminates it when it outlives the
   * timeout and closes its handle. The DPC stack lives until the dispatcher is destroyed.
   *
   * @param timeout How long to wait for the thread to finish its current work.
   */
  void Stop(std::chrono::milliseconds timeout);

 private:
  struct QueuedDPC {
    u32 address;
    u32 routine;
    u32 context;
    u32 arg1;
    u32 arg2;
  };

  struct DPCImpersonationScope {
    u8 previous_irql_;
  };

  DPCImpersonationScope BeginDPCImpersonation(X_KPCR& pcr);
  void EndDPCImpersonation(X_KPCR& pcr, const DPCImpersonationScope& scope);
  std::optional<QueuedDPC> PopDPC();
  void RunQueuedDPCs(ThreadState& state);
  void CompleteOverlappedEx(u32 overlapped_ptr, X_RESULT result, u32 extended_error, u32 length);

  Runtime& runtime_;
  u32 dpc_list_head_ = 0;
  u32 dpc_stack_base_ = 0;
  u32 dpc_stack_top_ = 0;
  std::atomic<bool> draining_{false};
  rex::platform::RankedMutex dpc_mutex_{rex::platform::LockRank::Leaf};
  rex::platform::RankedMutex dispatch_mutex_{rex::platform::LockRank::Leaf};
  std::condition_variable_any dispatch_cond_;
  std::list<std::function<void()>> dispatch_queue_;
  std::atomic<bool> dispatch_thread_running_{false};
  X_HANDLE dispatch_thread_ = 0;
  rex::platform::RankedMutex expiry_mutex_{rex::platform::LockRank::Leaf};
  std::vector<std::weak_ptr<rex::platform::TimerQueueWaitItem>> timer_expiries_;
  bool expiries_stopped_ = true;
};

}  // namespace rex::runtime
