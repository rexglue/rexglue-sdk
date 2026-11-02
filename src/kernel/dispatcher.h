/**
 * @file        kernel/dispatcher.h
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

#include <rex/kernel/types/thread.h>
#include <rex/platform.h>
#include <rex/types.h>
#include <rex/platform/lock_order.h>
#include <rex/platform/timer.h>
#include <rex/kernel/thread_registry.h>
#include <rex/utility.h>

namespace rex {
namespace kernel {
class TitleRuntime;
}  // namespace kernel
}  // namespace rex

namespace rex::kernel {

struct X_KPCR;
class ThreadState;

/** Marks the calling thread as running an ISR. */
void EnterInterrupt();

/**
 * Marks the end of an ISR on the calling thread.
 *
 * @return True when the outermost ISR ended.
 */
bool LeaveInterrupt();

class REX_KERNEL_API Dispatcher final : public IOverlappedDispatcher, public IDPCQueue {
 public:
  /**
   * Creates the dispatcher.
   *
   * @param runtime The runtime that owns the memory, object table and thread registry the
   * dispatcher uses.
   */
  explicit Dispatcher(TitleRuntime& runtime);
  ~Dispatcher() override;

  void Complete(u32 overlapped, const CompletionOptions& options) override;
  void Publish(rex::MoveOnlyFunction<void()> publish) override;
  u32 Observe(rex::MoveOnlyFunction<u32()> observe) override;
  void CompleteDeferred(u32 overlapped, rex::MoveOnlyFunction<void()> work,
                        const CompletionOptions& options, rex::MoveOnlyFunction<void()> pre = {},
                        rex::MoveOnlyFunction<void()> post = {}) override;
  void CompleteDeferred(u32 overlapped, rex::MoveOnlyFunction<CompletionOptions()> work,
                        rex::MoveOnlyFunction<void()> pre = {},
                        rex::MoveOnlyFunction<void()> post = {}) override;
  bool Queue(u32 dpc, u32 arg1, u32 arg2) override;
  bool Remove(u32 dpc) override;
  void Drain() override;

  bool HasQueuedDPCs();

  /** Runs work on the dispatch thread. */
  void Post(rex::MoveOnlyFunction<void()> work);

  /** Drains the DPC queue on the dispatch thread, standing in for the clock interrupt. */
  void PostDrain();

  bool IsDispatchThread() const;

  /** Reports whether the dispatch thread runs. */
  bool IsRunning() const;

  /**
   * @return A pin on the thread, empty when it is not running.
   */
  ThreadPin FindThread() const;

  /**
   * Registers a KTIMER expiry that Stop disarms.
   *
   * @return False when the dispatcher is stopped, and the caller disarms the expiry.
   */
  bool TrackTimerExpiry(std::weak_ptr<rex::platform::TimerQueueWaitItem> expiry);

  /** Allocates the DPC stack and starts the dispatch thread unless it runs. */
  void Start();

  /** Asks the dispatch thread to stop at its next wait or kernel call and returns at once. */
  void RequestStop();

  /**
   * Disarms the KTIMER expiries, stops the dispatch thread, waits for it and closes its handle.
   * The DPC stack stays allocated until the dispatcher is destroyed.
   *
   * @param timeout The time between reports of a running dispatch thread.
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
    u8 previous_irql;
  };

  DPCImpersonationScope BeginDPCImpersonation(X_KPCR& pcr);
  void EndDPCImpersonation(X_KPCR& pcr, const DPCImpersonationScope& scope);
  std::optional<QueuedDPC> PopDPC();
  void RunQueuedDPCs(ThreadState& state);
  void CompleteOverlappedEx(u32 overlapped, X_RESULT result, u32 extended_error, u32 length);

  TitleRuntime& runtime_;
  u32 dpc_list_head_ = 0;
  u32 dpc_stack_base_ = 0;
  u32 dpc_stack_top_ = 0;
  std::atomic<bool> draining_{false};
  rex::platform::RankedMutex dpc_mutex_{rex::platform::lock_rank::kDPC};
  rex::platform::RankedMutex dispatch_mutex_{rex::platform::LockRank::kLeaf};
  std::condition_variable_any dispatch_cond_;
  std::list<rex::MoveOnlyFunction<void()>> dispatch_queue_;
  std::atomic<bool> dispatch_thread_running_{false};
  X_HANDLE dispatch_thread_ = 0;
  rex::platform::RankedMutex expiry_mutex_{rex::platform::LockRank::kLeaf};
  std::vector<std::weak_ptr<rex::platform::TimerQueueWaitItem>> timer_expiries_;
  bool expiries_stopped_ = true;
};

}  // namespace rex::kernel
