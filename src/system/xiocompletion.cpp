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

#include <rex/system/xiocompletion.h>

#include <chrono>
#include <cstring>

#include <rex/assert.h>

namespace rex::system {

IOCompletionObject::IOCompletionObject() {
  notification_semaphore = rex::platform::Semaphore::Create(0, kMaxNotifications);
  assert_not_null(notification_semaphore);
}

void IOCompletionQueue(IOCompletionObject& port, const IONotification& notification) {
  std::unique_lock<std::mutex> lock(port.notification_lock);

  port.notifications.push(notification);
  port.notification_semaphore->Release(1, nullptr);
}

bool IOCompletionWait(IOCompletionObject& port, u64 wait_ticks, IONotification* notify) {
  auto ms = std::chrono::milliseconds(TimeoutTicksToMs(static_cast<i64>(wait_ticks)));
  auto res = rex::platform::Wait(port.notification_semaphore.get(), false, ms);
  if (res == rex::platform::WaitResult::kSuccess) {
    std::unique_lock<std::mutex> lock(port.notification_lock);
    assert_false(port.notifications.empty());

    std::memcpy(notify, &port.notifications.front(), sizeof(IONotification));
    port.notifications.pop();

    return true;
  }

  return false;
}

}  // namespace rex::system
