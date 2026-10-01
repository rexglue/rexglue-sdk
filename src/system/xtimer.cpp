/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <functional>

#include <rex/chrono/chrono.h>
#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_registry.h>
#include <rex/system/xthread.h>
#include <rex/system/xtimer.h>

namespace rex::system {

TimerObject::TimerObject(u32 timer_type) {
  switch (timer_type) {
    case 0:
      timer = rex::platform::Timer::CreateManualResetTimer();
      break;
    case 1:
      timer = rex::platform::Timer::CreateSynchronizationTimer();
      break;
    default:
      assert_always();
      break;
  }
  assert_not_null(timer);
}

X_STATUS TimerSet(TimerObject& timer, KernelState* kernel_state, i64 due_time, u32 period_ms,
                  u32 routine, u32 routine_arg, bool resume) {
  using rex::WinSystemClock;
  using rex::XSystemClock;
  if (resume) {
    return X_STATUS_TIMER_RESUME_IGNORED;
  }

  period_ms = Clock::ScaleGuestDurationMillis(period_ms);
  WinSystemClock::time_point due_tp;
  if (due_time < 0) {
    auto after = rex::hundrednanoseconds(-due_time);
    due_tp = std::chrono::clock_cast<WinSystemClock>(XSystemClock::now() + after);
  } else {
    due_tp = std::chrono::clock_cast<WinSystemClock>(XSystemClock::from_file_time(due_time));
  }

  timer.callback_thread_id = ThreadRecord::GetCurrentThread()->thread_id();
  timer.callback_routine = routine;
  timer.callback_routine_arg = routine_arg;

  std::function<void()> callback = nullptr;
  if (routine) {
    callback = [kernel_state, thread_id = timer.callback_thread_id, routine, routine_arg]() {
      const u64 time = rex::Clock::QueryGuestSystemTime();
      const u32 time_low = static_cast<u32>(time);
      const u32 time_high = static_cast<u32>(time >> 32);
      REXLOG_DEBUG(system, "timer enqueuing callback to {:08X}({:08X}, {:08X}, {:08X})", routine,
                   routine_arg, time_low, time_high);
      if (auto thread = kernel_state->threads()->Find(thread_id)) {
        thread->EnqueueApc(routine, routine_arg, time_low, time_high);
      }
    };
  }

  const bool result = period_ms
                          ? timer.timer->SetRepeatingAt(
                                due_tp, std::chrono::milliseconds(period_ms), std::move(callback))
                          : timer.timer->SetOnceAt(due_tp, std::move(callback));
  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

X_STATUS TimerCancel(TimerObject& timer) {
  return timer.timer->Cancel() ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

}  // namespace rex::system
