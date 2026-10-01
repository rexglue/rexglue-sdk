#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <memory>
#include <mutex>
#include <queue>

#include <rex/platform/thread.h>
#include <rex/runtime/xobject.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

/** One completed request queued on an IO completion port. */
struct IONotification {
  u32 key_context;
  u32 apc_context;
  u32 status;
  u32 num_bytes;
};

/** The host state of an IO completion port. */
struct IOCompletionObject {
  static constexpr u32 kMaxNotifications = 1024;

  IOCompletionObject();

  std::mutex notification_lock;
  std::queue<IONotification> notifications;
  std::unique_ptr<rex::platform::Semaphore> notification_semaphore;
};

/**
 * Queues a notification and wakes one waiter.
 *
 * @param port The port.
 * @param notification The notification to queue.
 */
void IOCompletionQueue(IOCompletionObject& port, const IONotification& notification);

/**
 * Waits for a notification and dequeues it.
 *
 * @param port The port.
 * @param wait_ticks The timeout in 100 nanosecond ticks, negative for relative.
 * @param notify Receives the notification when one arrived.
 * @return True when the wait ended because a notification was received.
 */
bool IOCompletionWait(IOCompletionObject& port, u64 wait_ticks, IONotification* notify);

}  // namespace rex::runtime
