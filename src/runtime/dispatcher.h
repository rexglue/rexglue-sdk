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
#include <condition_variable>
#include <functional>
#include <list>

#include <rex/runtime/dispatcher.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

/** The dispatcher of a runtime and its dispatch thread. */
class Dispatcher final : public IDispatcher {
 public:
  /**
   * @param runtime The runtime whose memory, table and thread registry completions use.
   */
  explicit Dispatcher(Runtime& runtime);

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
  util::NativeList& dpc_list() override { return dpc_list_; }
  rex::platform::RankedMutex& dpc_mutex() override { return dpc_mutex_; }

  /** Starts the dispatch thread unless it runs. */
  void Start();

  /** Stops the dispatch thread and closes its handle. */
  void Stop();

 private:
  /** The IRQL a DPC impersonation raised from. */
  struct DPCImpersonationScope {
    u8 previous_irql_;
  };

  /**
   * Raises the current guest thread to DISPATCH_LEVEL and marks a DPC active.
   *
   * @return The scope that EndDPCImpersonation restores.
   */
  DPCImpersonationScope BeginDPCImpersonation();

  /**
   * Restores the IRQL a DPC impersonation raised from and clears the DPC flag.
   *
   * @param scope The scope BeginDPCImpersonation returned.
   */
  void EndDPCImpersonation(const DPCImpersonationScope& scope);

  void CompleteOverlappedEx(u32 overlapped_ptr, X_RESULT result, u32 extended_error, u32 length);

  Runtime& runtime_;
  util::NativeList dpc_list_;
  rex::platform::RankedMutex dpc_mutex_{rex::platform::LockRank::Leaf};
  rex::platform::RankedMutex dispatch_mutex_{rex::platform::LockRank::Leaf};
  std::condition_variable_any dispatch_cond_;
  std::list<std::function<void()>> dispatch_queue_;
  std::atomic<bool> dispatch_thread_running_{false};
  X_HANDLE dispatch_thread_ = 0;
};

}  // namespace rex::runtime
