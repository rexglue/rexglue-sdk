/**
 * @file        runtime/dispatcher.h
 * @brief       The overlapped completion and DPC list interface
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <functional>

#include <rex/macros.h>
#include <rex/runtime/xtypes.h>
#include <rex/types.h>

namespace rex::runtime {

/**
 * Completes overlapped operations on the calling thread or on the dispatch thread, and holds the
 * DPC list.
 */
class IDispatcher {
 public:
  REX_INTERFACE(IDispatcher, "rex.runtime.dispatcher", 1);
  virtual ~IDispatcher() = default;

  /**
   * Completes an overlapped operation on the calling thread, with length 0 on success and
   * 0xFFFFFFFF on failure.
   *
   * @param overlapped_ptr The guest XOVERLAPPED.
   * @param result The result.
   */
  virtual void CompleteOverlappedImmediate(u32 overlapped_ptr, X_RESULT result) = 0;

  /**
   * Completes an overlapped operation on the calling thread.
   *
   * @param overlapped_ptr The guest XOVERLAPPED.
   * @param result The result.
   * @param extended_error The extended error.
   * @param length The length.
   */
  virtual void CompleteOverlappedImmediateEx(u32 overlapped_ptr, X_RESULT result,
                                             u32 extended_error, u32 length) = 0;

  /**
   * Marks an overlapped operation pending and completes it on the dispatch thread after the
   * callback ran.
   *
   * @param completion_callback The work.
   * @param overlapped_ptr The guest XOVERLAPPED.
   * @param result The result.
   * @param pre_callback Runs before the work, may be empty.
   * @param post_callback Runs after the completion, may be empty.
   */
  virtual void CompleteOverlappedDeferred(std::function<void()> completion_callback,
                                          u32 overlapped_ptr, X_RESULT result,
                                          std::function<void()> pre_callback = nullptr,
                                          std::function<void()> post_callback = nullptr) = 0;

  /**
   * Marks an overlapped operation pending and completes it on the dispatch thread after the
   * callback ran.
   *
   * @param completion_callback The work.
   * @param overlapped_ptr The guest XOVERLAPPED.
   * @param result The result.
   * @param extended_error The extended error.
   * @param length The length.
   * @param pre_callback Runs before the work, may be empty.
   * @param post_callback Runs after the completion, may be empty.
   */
  virtual void CompleteOverlappedDeferredEx(std::function<void()> completion_callback,
                                            u32 overlapped_ptr, X_RESULT result, u32 extended_error,
                                            u32 length,
                                            std::function<void()> pre_callback = nullptr,
                                            std::function<void()> post_callback = nullptr) = 0;

  /**
   * Marks an overlapped operation pending and completes it on the dispatch thread with the result
   * the callback returns.
   *
   * @param completion_callback The work, returning the result.
   * @param overlapped_ptr The guest XOVERLAPPED.
   * @param pre_callback Runs before the work, may be empty.
   * @param post_callback Runs after the completion, may be empty.
   */
  virtual void CompleteOverlappedDeferred(std::function<X_RESULT()> completion_callback,
                                          u32 overlapped_ptr,
                                          std::function<void()> pre_callback = nullptr,
                                          std::function<void()> post_callback = nullptr) = 0;

  /**
   * Marks an overlapped operation pending and completes it on the dispatch thread with the
   * result, extended error and length the callback returns.
   *
   * @param completion_callback The work, writing the extended error and length.
   * @param overlapped_ptr The guest XOVERLAPPED.
   * @param pre_callback Runs before the work, may be empty.
   * @param post_callback Runs after the completion, may be empty.
   */
  virtual void CompleteOverlappedDeferredEx(std::function<X_RESULT(u32&, u32&)> completion_callback,
                                            u32 overlapped_ptr,
                                            std::function<void()> pre_callback = nullptr,
                                            std::function<void()> post_callback = nullptr) = 0;

  /**
   * Queues a DPC unless it is queued. The caller drains when its IRQL allows it.
   *
   * @param dpc Address of the title's KDPC.
   * @param arg1 SystemArgument1.
   * @param arg2 SystemArgument2.
   * @return True when the DPC was queued, false when it was already queued.
   */
  virtual bool QueueDPC(u32 dpc, u32 arg1, u32 arg2) = 0;

  /**
   * Removes a queued DPC.
   *
   * @param dpc Address of the title's KDPC.
   * @return True when the DPC was queued.
   */
  virtual bool RemoveDPC(u32 dpc) = 0;

  /**
   * Runs every queued DPC on the calling thread at DISPATCH_LEVEL, 1 at a time, on the DPC stack.
   * Returns at once inside an ISR, inside a DPC, on a thread with no title state, or while another
   * thread drains.
   */
  virtual void DrainDPCs() = 0;
};

}  // namespace rex::runtime
