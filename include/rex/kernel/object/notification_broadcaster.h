/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        kernel/object/notification_broadcaster.h
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * @remarks     Parts derived from Xenia (Ben Vanik, 2022).
 * @remarks     Parts derived from Xenia (Ben Vanik, 2015).
 * @remarks     Parts derived from Xenia (Ben Vanik, 2020).
 */

#pragma once

#include <rex/assert.h>
#include <rex/kernel/types/status.h>
#include <rex/module.h>
#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::kernel {

/** ID of a notification, split into fields by `XNotificationKey`. */
using XNotificationID = u32;

REX_DECLARE_INTERFACE(INotificationBroadcaster, 1)
  virtual ~INotificationBroadcaster() = default;

  /**
   * Creates a notify listener and registers it. The notifier keeps its own duplicate of the
   * handle, and the first listener with the system mask receives the startup notifications.
   *
   * @param mask The bit mask of notification areas the listener receives.
   * @param max_version The highest notification version the listener receives.
   * @return The handle of the listener, or the status of its creation.
   */
  [[nodiscard]] virtual Result<X_HANDLE> AddNotifyListener(u64 mask, u32 max_version) = 0;

  /** Queues a notification on every registered listener whose mask accepts it. */
  virtual void BroadcastNotification(XNotificationID id, u32 data) = 0;
};

union XNotificationKey {
  XNotificationID id; /**< The whole notification ID, shares its 32 bits with the fields below. */
  struct {
    u32 local_id : 16;  /**< Bits 0 to 15. ID within the notification area. */
    u32 version : 9;    /**< Bits 16 to 24. Version of the notification. */
    u32 mask_index : 6; /**< Bits 25 to 30. Bit of the listener mask that selects the area. */
    u32 : 1;
  };

  constexpr XNotificationKey(XNotificationID notification_id = static_cast<XNotificationID>(0))
      : id(notification_id) {
    REX_STATIC_ASSERT_SIZE(*this, sizeof(id));
  }

  constexpr operator XNotificationID() const { return id; }
};

}  // namespace rex::kernel
