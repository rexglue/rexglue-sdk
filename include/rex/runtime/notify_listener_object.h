/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/macros.h>
#include <rex/platform/thread.h>
#include <rex/runtime/xcontent.h>
#include <rex/runtime/object.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

/** Splits a notification id into its bit fields. */
union XNotificationKey {
  XNotificationID id; /**< The whole notification id, shares its 32 bits with the fields below. */
  struct {
    u32 local_id : 16;  /**< Bits 0 to 15. Id within the notification area. */
    u32 version : 9;    /**< Bits 16 to 24. Version of the notification. */
    u32 mask_index : 6; /**< Bits 25 to 30. Bit of the listener mask that selects the area. */
    u32 : 1;
  };

  /**
   * Builds the key of a notification id.
   *
   * @param notification_id The id.
   */
  constexpr XNotificationKey(XNotificationID notification_id = XNotificationID(0))
      : id(notification_id) {
    REX_STATIC_ASSERT_SIZE(*this, sizeof(id));
  }

  constexpr operator XNotificationID() const { return id; }
};

/** The host state of a notification listener. */
struct NotifyListenerObject {
  /**
   * @param mask_in The bit mask of notification areas the listener receives.
   * @param max_version_in The highest notification version the listener receives.
   */
  REX_RUNTIME_API NotifyListenerObject(u64 mask_in, u32 max_version_in);

  /** Manual reset event set while notifications is not empty. */
  std::unique_ptr<rex::platform::Event> wait_handle;
  std::mutex notifications_lock; /**< Guards notifications. */
  /** Queued notifications in arrival order, each with its id and data. */
  std::vector<std::pair<XNotificationID, u32>> notifications;
  u64 mask = 0; /**< Bit mask of notification areas, bit N admits ids whose mask_index is N. */
  u32 max_version = 0; /**< Highest notification version admitted, a higher version is dropped. */
};

/**
 * Queues a notification when it matches the mask and version of the listener.
 *
 * @param listener The listener.
 * @param id The notification id.
 * @param data The notification data.
 */
REX_RUNTIME_API void NotifyListenerEnqueue(NotifyListenerObject& listener, XNotificationID id,
                                           u32 data);

/**
 * Removes the oldest notification.
 *
 * @param listener The listener.
 * @param out_id Receives the id.
 * @param out_data Receives the data.
 * @return True when a notification was removed.
 */
REX_RUNTIME_API bool NotifyListenerDequeue(NotifyListenerObject& listener, XNotificationID* out_id,
                                           u32* out_data);

/**
 * Removes the oldest notification with a given id.
 *
 * @param listener The listener.
 * @param id The id to look for.
 * @param out_data Receives the data.
 * @return True when a notification was removed.
 */
REX_RUNTIME_API bool NotifyListenerDequeueByID(NotifyListenerObject& listener, XNotificationID id,
                                               u32* out_data);

}  // namespace rex::runtime
