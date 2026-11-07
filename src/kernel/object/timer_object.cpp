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

#include <rex/chrono.h>
#include <rex/logging/log_category.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/kernel/object/timer_object.h>
#include <rex/platform/timer.h>

#include "kernel/dispatcher.h"
#include "kernel/title_state.h"

namespace rex::kernel {

namespace {

void Disarm(TimerObject& timer) {
  if (auto item = timer.expiry.lock()) {
    item->Disarm();
  }
  timer.expiry.reset();
}

bool QueueExpiry(TimerObject& timer, TitleRuntime& runtime, i64 due_time, u32 scaled_period,
                 std::function<void(void*)> callback) {
  using QueueClock = rex::platform::TimerQueueWaitItem::clock;
  const rex::HundredNanoseconds wait =
      std::max(rex::HundredNanoseconds::zero(),
               due_time < 0 ? rex::HundredNanoseconds(-due_time)
                            : rex::XSystemClock::FromFileTime(due_time) - rex::XSystemClock::now());
  const auto due = QueueClock::now() + std::chrono::duration_cast<QueueClock::duration>(wait);
  timer.expiry = scaled_period
                     ? rex::platform::QueueTimerRecurring(std::move(callback), nullptr, due,
                                                          std::chrono::milliseconds(scaled_period))
                     : rex::platform::QueueTimerOnce(std::move(callback), nullptr, due);
  if (!static_cast<Dispatcher&>(StateOf(runtime).GetDPCQueue()).TrackTimerExpiry(timer.expiry)) {
    Disarm(timer);
    return false;
  }
  return true;
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

X_STATUS TimerObject::Set(TitleRuntime& runtime, i64 due_time, u32 period_ms, u32 routine,
                          u32 routine_arg, bool resume) {
  if (resume) {
    return X_STATUS_TIMER_RESUME_IGNORED;
  }
  callback_thread_id = KernelThread::GetCallingThread()->GetThreadID();
  callback_routine = routine;
  callback_routine_arg = routine_arg;
  Disarm(*this);
  rex::platform::Timer* const host_timer = timer.get();
  if (!host_timer->Reset()) {
    return X_STATUS_UNSUCCESSFUL;
  }
  const u32 scaled_period = Clock::ScaleGuestDurationMillis(period_ms);
  auto callback = [runtime = &runtime, host = host_timer, thread_id = callback_thread_id, routine,
                   routine_arg](void*) {
    host->SetOnceAfter(rex::HundredNanoseconds(0));
    if (!routine) {
      return;
    }
    const u64 time = rex::Clock::QueryGuestSystemTime();
    const u32 time_low = static_cast<u32>(time);
    const u32 time_high = static_cast<u32>(time >> 32);
    auto& dispatcher = static_cast<Dispatcher&>(StateOf(*runtime).GetDPCQueue());
    if (!dispatcher.IsRunning()) {
      REXSYS_WARN("timer completion routine {:08X} dropped, the dispatcher is stopped", routine);
      return;
    }
    dispatcher.Post([runtime, thread_id, routine, routine_arg, time_low, time_high] {
      REXSYS_DEBUG("Timer enqueuing APC to {:08X}({:08X}, {:08X}, {:08X})", routine, routine_arg,
                   time_low, time_high);
      if (auto thread = runtime->GetThreadRegistry().Find(thread_id)) {
        thread->EnqueueAPC(routine, routine_arg, time_low, time_high);
      }
    });
  };
  if (!QueueExpiry(*this, runtime, due_time, scaled_period, std::move(callback))) {
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS TimerObject::SetDPC(TitleRuntime& runtime, u32 ktimer, i64 due_time, u32 period_ms,
                             u32 dpc) {
  Disarm(*this);
  rex::platform::Timer* const host_timer = timer.get();
  if (!host_timer->Reset()) {
    return X_STATUS_UNSUCCESSFUL;
  }
  const u32 scaled_period = Clock::ScaleGuestDurationMillis(period_ms);
  auto callback = [runtime = &runtime, host = host_timer, ktimer, dpc,
                   periodic = scaled_period != 0](void*) {
    auto* object = StateOf(*runtime).GetAddressSpace()->TranslateVirtual<X_KTIMER*>(ktimer);
    object->header.signal_state = 1;
    if (!periodic) {
      object->header.inserted = 0;
    }
    if (dpc) {
      const u64 time = rex::Clock::QueryGuestSystemTime();
      auto& dispatcher = static_cast<Dispatcher&>(StateOf(*runtime).GetDPCQueue());
      dispatcher.Queue(dpc, static_cast<u32>(time), static_cast<u32>(time >> 32));
      dispatcher.PostDrain();
    }
    host->SetOnceAfter(rex::HundredNanoseconds(0));
  };
  if (!QueueExpiry(*this, runtime, due_time, scaled_period, std::move(callback))) {
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS TimerObject::Cancel() {
  Disarm(*this);
  return timer->Cancel() ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

}  // namespace rex::kernel
