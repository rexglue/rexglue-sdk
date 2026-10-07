#pragma once
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

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/macros.h>
#include <rex/platform/thread.h>
#include <rex/runtime/xcontent.h>
#include <rex/runtime/xobject.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

union XNotificationKey {
  XNotificationID id;
  struct {
    uint32_t local_id : 16;
    uint32_t version : 9;
    uint32_t mask_index : 6;
    uint32_t : 1;
  };

  constexpr XNotificationKey(XNotificationID notification_id = XNotificationID(0))
      : id(notification_id) {
    REX_STATIC_ASSERT_SIZE(*this, sizeof(id));
  }

  constexpr operator XNotificationID() { return id; }
};

/** The host state of a notification listener. */
struct NotifyListenerObject {
  /**
   * @param mask_in The bit mask of notification areas the listener receives.
   * @param max_version_in The highest notification version the listener receives.
   */
  REX_RUNTIME_API NotifyListenerObject(u64 mask_in, u32 max_version_in);

  std::unique_ptr<rex::platform::Event> wait_handle;
  std::mutex notifications_lock;
  std::vector<std::pair<XNotificationID, u32>> notifications;
  u64 mask = 0;
  u32 max_version = 0;
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
