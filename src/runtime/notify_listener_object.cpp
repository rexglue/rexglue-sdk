/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/runtime/notify_listener_object.h>

#include <rex/assert.h>
#include <rex/logging/log.h>

namespace rex::runtime {

NotifyListenerObject::NotifyListenerObject(u64 mask_in, u32 max_version_in)
    : mask(mask_in), max_version(max_version_in) {
  wait_handle = rex::platform::Event::CreateManualResetEvent(false);
  REX_ASSERT(wait_handle != nullptr);
}

void NotifyListenerObject::Enqueue(XNotificationID id, u32 data) {
  auto key = XNotificationKey(id);
  if ((mask & static_cast<u64>(1ULL << key.mask_index)) == 0) {
    return;
  }
  if (key.version > max_version) {
    return;
  }
  std::lock_guard<std::mutex> lock(notifications_lock);
  notifications.push_back(std::pair<XNotificationID, u32>(id, data));
  wait_handle->Set();
}

bool NotifyListenerObject::Dequeue(XNotificationID* out_id, u32* out_data) {
  std::lock_guard<std::mutex> lock(notifications_lock);
  bool dequeued = false;
  if (!notifications.empty()) {
    dequeued = true;
    auto it = notifications.begin();
    *out_id = it->first;
    *out_data = it->second;
    notifications.erase(it);
    if (notifications.empty()) {
      wait_handle->Reset();
    }
  }
  return dequeued;
}

bool NotifyListenerObject::DequeueByID(XNotificationID id, u32* out_data) {
  std::lock_guard<std::mutex> lock(notifications_lock);
  if (notifications.empty()) {
    return false;
  }
  bool dequeued = false;
  for (auto it = notifications.begin(); it != notifications.end(); ++it) {
    if (it->first != id) {
      continue;
    }
    dequeued = true;
    *out_data = it->second;
    notifications.erase(it);
    if (notifications.empty()) {
      wait_handle->Reset();
    }
    break;
  }
  return dequeued;
}

}  // namespace rex::runtime
