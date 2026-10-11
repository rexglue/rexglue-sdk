/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <memory>

#include <rex/macros.h>
#include <rex/platform/thread.h>
#include <rex/platform/timer_queue.h>
#include <rex/runtime/xobject.h>
#include <rex/runtime/xtypes.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

struct XDPC;

/** Guest layout of a kernel timer. */
struct X_KTIMER {
  X_DISPATCH_HEADER header;
  be_u64 due_time;
  X_LIST_ENTRY table_bucket_entry;
  TypedGuestPointer<XDPC> dpc;
  be_u32 period;
};
REX_STATIC_ASSERT_SIZE(X_KTIMER, 0x28);

/** The host state of a timer object. */
struct TimerObject {
  /**
   * @param timer_type 0 for a notification timer, 1 for a synchronization timer.
   */
  REX_RUNTIME_API explicit TimerObject(u32 timer_type);
  /** Disarms the timer. */
  REX_RUNTIME_API ~TimerObject();

  /** Host timer that signals the object when it expires. */
  std::unique_ptr<rex::platform::Timer> timer;
  /** Pending expiry entry the dispatcher tracks, expired when the timer is cancelled. */
  std::weak_ptr<rex::platform::TimerQueueWaitItem> expiry;
  /** Id of the thread that armed the timer, the thread its completion routine runs on. */
  u32 callback_thread_id = 0;
  u32 callback_routine = 0;     /**< Guest address of the completion routine, 0 for none. */
  u32 callback_routine_arg = 0; /**< Argument passed to the completion routine. */
};

/**
 * Arms a timer. A completion routine runs as an APC on the calling thread.
 *
 * @param timer The timer.
 * @param runtime The runtime the call runs in.
 * @param due_time A negative relative time in 100 nanosecond ticks, or an absolute file time.
 * @param period_ms The repeat period in milliseconds, 0 for a single shot.
 * @param routine The title completion routine, 0 for none.
 * @param routine_arg The argument passed to the routine.
 * @param resume True to request a resume from suspend, which is ignored.
 * @return X_STATUS_SUCCESS, X_STATUS_TIMER_RESUME_IGNORED or X_STATUS_UNSUCCESSFUL.
 */
REX_RUNTIME_API X_STATUS TimerSet(TimerObject& timer, Runtime& runtime, i64 due_time, u32 period_ms,
                                  u32 routine, u32 routine_arg, bool resume);

/**
 * Arms a KTIMER on the timer queue. At expiry the KTIMER header is updated, its DPC is queued with
 * the expiry system time for the Kernel Dispatch thread to drain, and then the timer is signaled.
 *
 * @param timer Host state of the KTIMER.
 * @param runtime The runtime that owns the dispatcher.
 * @param ktimer Address of the title's KTIMER.
 * @param due_time A negative relative time in 100 nanosecond ticks, or an absolute file time.
 * @param period_ms The repeat period in milliseconds, 0 for a single shot.
 * @param dpc Address of the title's KDPC, 0 for none.
 * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
 */
REX_RUNTIME_API X_STATUS TimerSetDPC(TimerObject& timer, Runtime& runtime, u32 ktimer, i64 due_time,
                                     u32 period_ms, u32 dpc);

/**
 * Cancels a timer.
 *
 * @param timer The timer.
 * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
 */
REX_RUNTIME_API X_STATUS TimerCancel(TimerObject& timer);

}  // namespace rex::runtime
