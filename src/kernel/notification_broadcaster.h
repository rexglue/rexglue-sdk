/**
 * @file        kernel/notification_broadcaster.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <vector>

#include <rex/platform/lock_order.h>
#include <rex/kernel/object/notification_broadcaster.h>

#include "kernel/object_table.h"

namespace rex::kernel {

class NotificationBroadcaster final : public INotificationBroadcaster {
 public:
  explicit NotificationBroadcaster(ObjectTable& table);

  [[nodiscard]] Result<X_HANDLE> AddNotifyListener(u64 mask, u32 max_version) override;
  void BroadcastNotification(XNotificationID id, u32 data) override;

  /** Closes the duplicated listener handles. */
  void Shutdown();

 private:
  void RegisterNotifyListener(X_HANDLE listener_handle);

  ObjectTable& table_;
  rex::platform::RankedMutex mutex_{rex::platform::LockRank::kLeaf};
  std::vector<X_HANDLE> listeners_;
  bool has_notified_startup_ = false;
};

}  // namespace rex::kernel
