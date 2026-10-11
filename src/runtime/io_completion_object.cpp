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

#include <rex/runtime/io_completion_object.h>

#include <chrono>
#include <cstring>

#include <rex/assert.h>

#include "runtime/thread_wait.h"

namespace rex::runtime {

IOCompletionObject::IOCompletionObject() {
  notification_semaphore = rex::platform::Semaphore::Create(0, kMaxNotifications);
  REX_ASSERT(notification_semaphore != nullptr);
}

void IOCompletionQueue(IOCompletionObject& port, const IONotification& notification) {
  std::unique_lock<std::mutex> lock(port.notification_lock);

  port.notifications.push(notification);
  port.notification_semaphore->Release(1, nullptr);
}

bool IOCompletionWait(IOCompletionObject& port, u64 wait_ticks, IONotification* notify) {
  auto ms = TimeoutFromTicks(static_cast<i64>(wait_ticks));
  auto res = WaitHostObject(port.notification_semaphore.get(), ms);
  if (res == rex::platform::WaitResult::kSuccess) {
    std::unique_lock<std::mutex> lock(port.notification_lock);
    REX_ASSERT(!port.notifications.empty());

    *notify = port.notifications.front();
    port.notifications.pop();

    return true;
  }

  return false;
}

}  // namespace rex::runtime
