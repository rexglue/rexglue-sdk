/**
 * @file        runtime/notifier.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>
#include <rex/runtime/xcontent_data.h>
#include <rex/runtime/xtypes.h>
#include <rex/types.h>
#include <rex/module.h>

namespace rex::runtime {

/** Delivers system notifications to the notify listeners titles create. */
REX_INTERFACE(INotifier, "rex.runtime.notifier", 1)
 public:
  /**
   * Registers a notify listener. The notifier keeps its own duplicate of the handle, and the first
   * listener with the system mask receives the startup notifications.
   *
   * @param listener_handle The handle of a NotifyListenerObject.
   */
  virtual void RegisterNotifyListener(X_HANDLE listener_handle) = 0;

  /**
   * Queues a notification on every registered listener whose mask accepts it.
   *
   * @param id The identifier of the notification to queue.
   * @param data The parameter queued with the notification.
   */
  virtual void BroadcastNotification(XNotificationID id, u32 data) = 0;
};

}  // namespace rex::runtime
