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

#pragma once

#include <memory>
#include <mutex>
#include <queue>

#include <rex/macros.h>
#include <rex/platform/thread.h>
#include <rex/runtime/object.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

/** One completed request queued on an IO completion port. */
struct IONotification {
  u32 key_context; /**< Completion key of the port registration the file was bound with. */
  u32 apc_context; /**< APC context the title passed to the IO call. */
  u32 status;      /**< X_STATUS result of the IO call. */
  u32 num_bytes;   /**< Bytes the IO call transferred. */
};

/** The host state of an IO completion port. */
struct IOCompletionObject {
  /** Maximum count of the notification semaphore, so the queue holds at most this many entries. */
  static constexpr u32 kMaxNotifications = 1024;

  /** Creates the port with an empty queue. */
  REX_RUNTIME_API IOCompletionObject();

  std::mutex notification_lock; /**< Guards notifications. */
  /** Completed requests in arrival order, 1 per release of notification_semaphore. */
  std::queue<IONotification> notifications;
  /** Counts the queued notifications, waited on to receive the next one. */
  std::unique_ptr<rex::platform::Semaphore> notification_semaphore;

  /**
   * Queues a notification and wakes one waiter.
   *
   * @param notification The notification to queue.
   */
  REX_RUNTIME_API void Queue(const IONotification& notification);

  /**
   * Waits for a notification and dequeues it.
   *
   * @param wait_ticks The timeout in 100 nanosecond ticks, negative for relative.
   * @param notify Receives the notification when one arrived.
   * @return True when the wait ended because a notification was received.
   */
  REX_RUNTIME_API bool Wait(u64 wait_ticks, IONotification* notify);
};

}  // namespace rex::runtime
