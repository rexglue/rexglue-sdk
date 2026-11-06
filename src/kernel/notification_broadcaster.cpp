#include "kernel/notification_broadcaster.h"

#include <algorithm>
#include <mutex>

#include <rex/logging/log_category.h>
#include <rex/kernel/object/notification_broadcaster.h>
#include <rex/kernel/object/notify_listener_object.h>

namespace rex::kernel {

NotificationBroadcaster::NotificationBroadcaster(ObjectTable& table) : table_(table) {}

Result<X_HANDLE> NotificationBroadcaster::AddNotifyListener(u64 mask, u32 max_version) {
  ObjectBlock block = table_.AllocateObjectBlock(
      kNotifyListenerObjectSize, ObjectType::kNotifyListener, ObjectBlockHeap::kSystem);
  if (!block.address) {
    return Err<X_HANDLE>(StatusError(X_STATUS_NO_MEMORY));
  }
  Result<X_HANDLE> added =
      table_.Emplace<NotifyListenerObject>(AddOptions{{}, block}, mask, max_version);
  if (added) {
    if (auto listener = table_.Get<NotifyListenerObject>(*added)) {
      RegisterNotifyListener(listener.GetGuestAddress(), *listener);
    }
  }
  return added;
}

void NotificationBroadcaster::RegisterNotifyListener(u32 address, NotifyListenerObject& listener) {
  std::lock_guard lock(mutex_);
  listeners_.push_back(address);
  if (!has_notified_startup_ && listener.mask & 0x00000001) {
    has_notified_startup_ = true;
    listener.Enqueue(0x00000009, 1);
    listener.Enqueue(0x00000009, 0);
    listener.Enqueue(0x0000000A, 1);
    listener.Enqueue(0x0000000A, 1);
    listener.Enqueue(0x00000012, 0);
    listener.Enqueue(0x00000012, 0);
    listener.Enqueue(0x00000013, 0);
    listener.Enqueue(0x00000013, 0);
  }
}

void NotificationBroadcaster::BroadcastNotification(XNotificationID id, u32 data) {
  std::vector<u32> snapshot;
  {
    std::lock_guard lock(mutex_);
    REXSYS_DEBUG("BroadcastNotification(id={:#x}, data={}) to {} listeners", static_cast<u32>(id),
                 data, listeners_.size());
    snapshot = listeners_;
  }
  std::ranges::sort(snapshot);
  snapshot.erase(std::ranges::unique(snapshot).begin(), snapshot.end());
  std::vector<u32> gone;
  for (u32 address : snapshot) {
    if (auto listener = Pinned<NotifyListenerObject>(table_.GetByAddress(address))) {
      listener->Enqueue(id, data);
    } else {
      gone.push_back(address);
    }
  }
  if (!gone.empty()) {
    std::lock_guard lock(mutex_);
    for (u32 address : gone) {
      if (auto it = std::ranges::find(listeners_, address); it != listeners_.end()) {
        listeners_.erase(it);
      }
    }
  }
}

void NotificationBroadcaster::Shutdown() {
  std::lock_guard lock(mutex_);
  listeners_.clear();
}

}  // namespace rex::kernel
