/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        kernel/object/notify_listener_object.h
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
#include <utility>
#include <vector>

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
#include <rex/kernel/object/notifier.h>

namespace rex::kernel {

class INotifyListenerQueue {
 public:
  virtual ~INotifyListenerQueue() = default;

  /**
   * Removes the oldest notification with a given ID.
   *
   * @param out_data Receives the data.
   * @return True when a notification was removed.
   */
  virtual bool DequeueByID(XNotificationID id, u32* out_data) = 0;

  /**
   * Removes the oldest notification.
   *
   * @param out_id Receives the ID.
   * @param out_data Receives the data.
   * @return True when a notification was removed.
   */
  virtual bool Dequeue(XNotificationID* out_id, u32* out_data) = 0;
};

struct NotifyListenerObject : INotifyListenerQueue {
  /**
   * @param mask_in The bit mask of notification areas the listener receives.
   * @param max_version_in The highest notification version the listener receives.
   */
  REX_KERNEL_API NotifyListenerObject(u64 mask_in, u32 max_version_in);

  /** Manual reset event set while notifications is not empty. */
  std::unique_ptr<rex::platform::Event> wait_handle;
  std::mutex notifications_lock;
  /** Queued notifications in arrival order, each with its ID and data. */
  std::vector<std::pair<XNotificationID, u32>> notifications;
  u64 mask = 0; /**< Bit mask of notification areas, bit N admits IDs whose mask_index is N. */
  u32 max_version = 0; /**< Highest notification version admitted, a higher version is discarded. */

  /** Queues a notification when it matches the mask and version of the listener. */
  REX_KERNEL_API void Enqueue(XNotificationID id, u32 data);

  REX_KERNEL_API bool DequeueByID(XNotificationID id, u32* out_data) override;

  REX_KERNEL_API bool Dequeue(XNotificationID* out_id, u32* out_data) override;
};

}  // namespace rex::kernel
