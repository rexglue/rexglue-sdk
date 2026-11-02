/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        kernel/object/timer_object.h
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * @remarks     Parts derived from Xenia (Ben Vanik, 2022).
 * @remarks     Parts derived from Xenia (Ben Vanik, 2015).
 * @remarks     Parts derived from Xenia (Ben Vanik, 2020).
 */

#pragma once

#include <memory>

#include <rex/assert.h>
#include <rex/kernel/object/object.h>
#include <rex/kernel/types/list.h>
#include <rex/kernel/types/memory.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/types/thread.h>
#include <rex/module.h>
#include <rex/platform.h>
#include <rex/platform/sync.h>
#include <rex/platform/thread.h>
#include <rex/platform/timer.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex {
namespace kernel {
class TitleRuntime;
}  // namespace kernel
}  // namespace rex

namespace rex::kernel {

struct TimerObject {
  /**
   * Constructs a timer of a type.
   *
   * @param timer_type 0 for a notification timer, 1 for a synchronization timer.
   */
  REX_KERNEL_API explicit TimerObject(u32 timer_type);
  /** Disarms the timer. */
  REX_KERNEL_API ~TimerObject();

  /** Host timer that signals the object when it expires. */
  std::unique_ptr<rex::platform::Timer> timer;
  /** Pending expiry entry the dispatcher tracks, expired when the timer is canceled. */
  std::weak_ptr<rex::platform::TimerQueueWaitItem> expiry;
  /** ID of the thread that armed the timer, the thread its completion routine runs on. */
  u32 callback_thread_id = 0;
  u32 callback_routine = 0; /**< Guest address of the completion routine, 0 for none. */
  u32 callback_routine_arg = 0;

  /**
   * Arms a KTIMER that queues its DPC when it expires.
   *
   * @param runtime The runtime that owns the dispatcher.
   * @param ktimer Address of the title's KTIMER.
   * @param due_time A negative relative time in 100 nanosecond ticks, or an absolute file time.
   * @param period_ms The repeat period in milliseconds, 0 for a single shot.
   * @param dpc Address of the title's KDPC, 0 for none.
   * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
   */
  REX_KERNEL_API X_STATUS SetDPC(TitleRuntime& runtime, u32 ktimer, i64 due_time, u32 period_ms,
                                 u32 dpc);

  /**
   * Arms a timer. A completion routine runs as an APC on the calling thread.
   *
   * @param due_time A negative relative time in 100 nanosecond ticks, or an absolute file time.
   * @param period_ms The repeat period in milliseconds, 0 for a single shot.
   * @param routine The title completion routine, 0 for none.
   * @param resume True to request a resume from suspend, which is ignored.
   * @return X_STATUS_SUCCESS, X_STATUS_TIMER_RESUME_IGNORED or X_STATUS_UNSUCCESSFUL.
   */
  REX_KERNEL_API X_STATUS Set(TitleRuntime& runtime, i64 due_time, u32 period_ms, u32 routine,
                              u32 routine_arg, bool resume);

  /**
   * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
   */
  REX_KERNEL_API X_STATUS Cancel();
};

}  // namespace rex::kernel
