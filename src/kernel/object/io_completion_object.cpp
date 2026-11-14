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

#include <rex/kernel/object/io_completion_object.h>

#include <chrono>
#include <cstring>

#include <rex/assert.h>
#include <rex/platform/sync.h>

#include "kernel/thread_wait.h"

namespace rex::kernel {

IOCompletionObject::IOCompletionObject() {
  notification_semaphore = rex::platform::Semaphore::Create(0, kMaxNotifications);
  REX_ASSERT(notification_semaphore != nullptr);
}

void IOCompletionObject::Queue(const IONotification& notification) {
  std::unique_lock<std::mutex> lock(notification_lock);

  notifications.push(notification);
  notification_semaphore->Release(1, nullptr);
}

bool IOCompletionObject::Wait(u64 wait_ticks, IONotification* notify) {
  auto ms = TimeoutFromTicks(static_cast<i64>(wait_ticks));
  auto res = WaitHostObject(notification_semaphore.get(), ms);
  if (res == rex::platform::WaitResult::kSuccess) {
    std::unique_lock<std::mutex> lock(notification_lock);
    REX_ASSERT(!notifications.empty());

    *notify = notifications.front();
    notifications.pop();

    return true;
  }

  return false;
}

}  // namespace rex::kernel
