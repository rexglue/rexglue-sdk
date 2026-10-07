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

#include <vector>

#include <rex/platform/lock_order.h>
#include <rex/runtime/notifier.h>

#include "runtime/object_table.h"

namespace rex::runtime {

/** The notifier of a runtime. */
class Notifier final : public INotifier {
 public:
  /**
   * @param table The table that holds the listener handles.
   */
  explicit Notifier(ObjectTable& table);

  void RegisterNotifyListener(X_HANDLE listener_handle) override;
  void BroadcastNotification(XNotificationID id, u32 data) override;

  /** Closes the duplicated listener handles. */
  void Shutdown();

 private:
  ObjectTable& table_;
  rex::platform::RankedMutex mutex_{rex::platform::LockRank::Leaf};
  std::vector<X_HANDLE> listeners_;
  bool has_notified_startup_ = false;
};

}  // namespace rex::runtime
