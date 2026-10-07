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

#include <algorithm>
#include <functional>

#include <rex/chrono/chrono.h>
#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/runtime/thread_registry.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtimer.h>

#include "runtime/dispatcher.h"

namespace rex::runtime {

namespace {

rex::WinSystemClock::time_point DueTimePoint(i64 due_time) {
  if (due_time < 0) {
    return std::chrono::clock_cast<rex::WinSystemClock>(rex::XSystemClock::now() +
                                                        rex::hundrednanoseconds(-due_time));
  }
  return std::chrono::clock_cast<rex::WinSystemClock>(rex::XSystemClock::from_file_time(due_time));
}

bool Arm(TimerObject& timer, i64 due_time, u32 period_ms, std::function<void()> callback) {
  const auto due = DueTimePoint(due_time);
  return period_ms ? timer.timer->SetRepeatingAt(due, std::chrono::milliseconds(period_ms),
                                                 std::move(callback))
                   : timer.timer->SetOnceAt(due, std::move(callback));
}

void Disarm(TimerObject& timer) {
  if (auto item = timer.expiry.lock()) {
    item->Disarm();
  }
  timer.expiry.reset();
}

}  // namespace

TimerObject::TimerObject(u32 timer_type) {
  switch (timer_type) {
    case 0:
      timer = rex::platform::Timer::CreateManualResetTimer();
      break;
    case 1:
      timer = rex::platform::Timer::CreateSynchronizationTimer();
      break;
    default:
      REX_ASSERT_ALWAYS();
      break;
  }
  REX_ASSERT(timer != nullptr);
}

TimerObject::~TimerObject() {
  Disarm(*this);
}

X_STATUS TimerSet(TimerObject& timer, Runtime& runtime, i64 due_time, u32 period_ms, u32 routine,
                  u32 routine_arg, bool resume) {
  if (resume) {
    return X_STATUS_TIMER_RESUME_IGNORED;
  }
  timer.callback_thread_id = ThreadRecord::GetCurrentThread()->thread_id();
  timer.callback_routine = routine;
  timer.callback_routine_arg = routine_arg;
  std::function<void()> callback = nullptr;
  if (routine) {
    callback = [runtime = &runtime, thread_id = timer.callback_thread_id, routine, routine_arg]() {
      const u64 time = rex::Clock::QueryGuestSystemTime();
      const u32 time_low = static_cast<u32>(time);
      const u32 time_high = static_cast<u32>(time >> 32);
      REXLOG_DEBUG(system, "timer enqueuing callback to {:08X}({:08X}, {:08X}, {:08X})", routine,
                   routine_arg, time_low, time_high);
      if (auto thread = runtime->thread_registry().Find(thread_id)) {
        thread->EnqueueAPC(routine, routine_arg, time_low, time_high);
      }
    };
  }
  return Arm(timer, due_time, Clock::ScaleGuestDurationMillis(period_ms), std::move(callback))
             ? X_STATUS_SUCCESS
             : X_STATUS_UNSUCCESSFUL;
}

X_STATUS TimerSetDPC(TimerObject& timer, Runtime& runtime, u32 ktimer, i64 due_time, u32 period_ms,
                     u32 dpc) {
  Disarm(timer);
  if (!timer.timer->Reset()) {
    return X_STATUS_UNSUCCESSFUL;
  }
  const u32 scaled_period = Clock::ScaleGuestDurationMillis(period_ms);
  auto callback = [runtime = &runtime, host = timer.timer.get(), ktimer, dpc,
                   periodic = scaled_period != 0](void*) {
    auto* object = runtime->memory()->TranslateVirtual<X_KTIMER*>(ktimer);
    object->header.signal_state = 1;
    if (!periodic) {
      object->header.inserted = 0;
    }
    if (dpc) {
      const u64 time = rex::Clock::QueryGuestSystemTime();
      auto& dispatcher = static_cast<Dispatcher&>(runtime->dispatcher());
      dispatcher.QueueDPC(dpc, static_cast<u32>(time), static_cast<u32>(time >> 32));
      dispatcher.PostDrain();
    }
    host->SetOnceAfter(rex::hundrednanoseconds(0));
  };
  using QueueClock = rex::platform::TimerQueueWaitItem::clock;
  const rex::hundrednanoseconds wait = std::max(
      rex::hundrednanoseconds::zero(),
      due_time < 0 ? rex::hundrednanoseconds(-due_time)
                   : rex::XSystemClock::from_file_time(due_time) - rex::XSystemClock::now());
  const auto due = QueueClock::now() + std::chrono::duration_cast<QueueClock::duration>(wait);
  timer.expiry = scaled_period
                     ? rex::platform::QueueTimerRecurring(std::move(callback), nullptr, due,
                                                          std::chrono::milliseconds(scaled_period))
                     : rex::platform::QueueTimerOnce(std::move(callback), nullptr, due);
  if (!static_cast<Dispatcher&>(runtime.dispatcher()).TrackTimerExpiry(timer.expiry)) {
    Disarm(timer);
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS TimerCancel(TimerObject& timer) {
  Disarm(timer);
  return timer.timer->Cancel() ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

}  // namespace rex::runtime
