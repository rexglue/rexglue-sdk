#pragma once
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

#include <memory>

#include <rex/platform/thread.h>
#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>

namespace rex::system {

struct XDPC;

struct X_KTIMER {
  X_DISPATCH_HEADER header;         // 0x0
  rex::be<uint64_t> due_time;       // 0x10
  X_LIST_ENTRY table_bucket_entry;  // 0x18
  TypedGuestPointer<XDPC> dpc;      // 0x20
  rex::be<uint32_t> period;         // 0x24
};
static_assert_size(X_KTIMER, 0x28);

/** The host state of a timer object. */
struct TimerObject {
  /**
   * @param timer_type 0 for a notification timer, 1 for a synchronization timer.
   */
  explicit TimerObject(u32 timer_type);

  std::unique_ptr<rex::platform::Timer> timer;
  u32 callback_thread_id = 0;
  u32 callback_routine = 0;
  u32 callback_routine_arg = 0;
};

/**
 * Arms a timer. A completion routine runs as an APC on the calling thread.
 *
 * @param timer The timer.
 * @param kernel_state The kernel state that owns the thread registry.
 * @param due_time A negative relative time in 100 nanosecond ticks, or an absolute file time.
 * @param period_ms The repeat period in milliseconds, 0 for a single shot.
 * @param routine The title completion routine, 0 for none.
 * @param routine_arg The argument passed to the routine.
 * @param resume True to request a resume from suspend, which is ignored.
 * @return X_STATUS_SUCCESS, X_STATUS_TIMER_RESUME_IGNORED or X_STATUS_UNSUCCESSFUL.
 */
X_STATUS TimerSet(TimerObject& timer, KernelState* kernel_state, i64 due_time, u32 period_ms,
                  u32 routine, u32 routine_arg, bool resume);

/**
 * Cancels a timer.
 *
 * @param timer The timer.
 * @return X_STATUS_SUCCESS, or X_STATUS_UNSUCCESSFUL.
 */
X_STATUS TimerCancel(TimerObject& timer);

}  // namespace rex::system
