/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        kernel/object/io_completion_object.h
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * @remarks     Parts derived from Xenia (Ben Vanik, 2022).
 * @remarks     Parts derived from Xenia (Ben Vanik, 2015).
 * @remarks     Parts derived from Xenia (Ben Vanik, 2020).
 */

#pragma once

#include <memory>
#include <mutex>
#include <queue>

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

namespace rex::kernel {

/** A completed request queued on an IO completion port. */
struct IONotification {
  u32 key;         /**< Completion key of the port registration the file was bound with. */
  u32 apc_context; /**< APC context the title passed to the IO call. */
  u32 status;      /**< X_STATUS result of the IO call. */
  u32 num_bytes;
};

struct IOCompletionObject {
  /** Maximum count of the notification semaphore, so the queue holds at most this many entries. */
  static constexpr u32 kMaxNotifications = 1024;

  REX_KERNEL_API IOCompletionObject();

  std::mutex notification_lock;
  /** Completed requests in arrival order, one per release of notification_semaphore. */
  std::queue<IONotification> notifications;
  /** Counts the queued notifications, waited on to receive the next one. */
  std::unique_ptr<rex::platform::Semaphore> notification_semaphore;

  /** Queues a notification and wakes one waiter. */
  REX_KERNEL_API void Queue(const IONotification& notification);

  /**
   * Waits for a notification and dequeues it.
   *
   * @param wait_ticks The timeout in 100 nanosecond ticks, negative for relative.
   * @param out_notification Receives the notification when one arrived.
   * @return True when the wait ended because a notification was received.
   */
  REX_KERNEL_API bool Wait(u64 wait_ticks, IONotification* out_notification);
};

}  // namespace rex::kernel
