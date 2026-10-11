/**
 * @file        runtime/dispatcher.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <optional>

#include <rex/macros.h>
#include <rex/move_only_function.h>
#include <rex/runtime/xtypes.h>
#include <rex/types.h>

namespace rex::runtime {

/**
 * The values an overlapped completion writes. An empty extended error is the result, and an empty
 * length is 0 on success and 0xFFFFFFFF on failure.
 */
struct CompletionOptions {
  X_RESULT result; /**< The result. */
  std::optional<u32> extended_error =
      std::nullopt;                         /**< The extended error, empty for the result. */
  std::optional<u32> length = std::nullopt; /**< The length, empty for the default of the result. */
};

/**
 * Completes overlapped operations on the calling thread or on the dispatch thread, and holds the
 * DPC list.
 */
class IDispatcher {
 public:
  REX_INTERFACE(IDispatcher, "rex.runtime.dispatcher", 1);
  virtual ~IDispatcher() = default;

  /**
   * Completes an overlapped operation on the calling thread.
   *
   * @param overlapped The guest XOVERLAPPED.
   * @param options The result, extended error and length to write.
   */
  virtual void Complete(u32 overlapped, const CompletionOptions& options) = 0;

  /**
   * Marks an overlapped operation pending and completes it on the dispatch thread after the work
   * ran.
   *
   * @param overlapped The guest XOVERLAPPED.
   * @param work The work.
   * @param options The result, extended error and length to write.
   * @param pre Runs before the work, may be empty.
   * @param post Runs after the completion, may be empty.
   */
  virtual void CompleteDeferred(u32 overlapped, rex::MoveOnlyFunction<void()> work,
                                const CompletionOptions& options,
                                rex::MoveOnlyFunction<void()> pre = {},
                                rex::MoveOnlyFunction<void()> post = {}) = 0;

  /**
   * Marks an overlapped operation pending and completes it on the dispatch thread with the options
   * the work returns.
   *
   * @param overlapped The guest XOVERLAPPED.
   * @param work The work, returning the result, extended error and length to write.
   * @param pre Runs before the work, may be empty.
   * @param post Runs after the completion, may be empty.
   */
  virtual void CompleteDeferred(u32 overlapped, rex::MoveOnlyFunction<CompletionOptions()> work,
                                rex::MoveOnlyFunction<void()> pre = {},
                                rex::MoveOnlyFunction<void()> post = {}) = 0;

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
