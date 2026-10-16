#include "runtime/notifier.h"

#include <mutex>

#include <rex/logging/log.h>
#include <rex/runtime/notify_listener_object.h>

namespace rex::runtime {

Notifier::Notifier(ObjectTable& table) : table_(table) {}

void Notifier::RegisterNotifyListener(X_HANDLE listener_handle) {
  Result<X_HANDLE> owned = table_.Duplicate(listener_handle);
  if (!owned) {
    return;
  }
  auto listener = table_.Get<NotifyListenerObject>(*owned);
  std::lock_guard lock(mutex_);
  listeners_.push_back(*owned);
  if (!has_notified_startup_ && listener->mask & 0x00000001) {
    has_notified_startup_ = true;
    listener->Enqueue(0x00000009, 1);
    listener->Enqueue(0x00000009, 0);
    listener->Enqueue(0x0000000A, 1);
    listener->Enqueue(0x0000000A, 1);
    listener->Enqueue(0x00000012, 0);
    listener->Enqueue(0x00000012, 0);
    listener->Enqueue(0x00000013, 0);
    listener->Enqueue(0x00000013, 0);
  }
}

void Notifier::BroadcastNotification(XNotificationID id, u32 data) {
  std::vector<X_HANDLE> snapshot;
  {
    std::lock_guard lock(mutex_);
    REXLOG_DEBUG(system, "BroadcastNotification(id={:#x}, data={}) to {} listeners",
                 static_cast<u32>(id), data, listeners_.size());
    snapshot = listeners_;
  }
  for (X_HANDLE handle : snapshot) {
    if (auto listener = table_.Get<NotifyListenerObject>(handle)) {
      listener->Enqueue(id, data);
    }
  }
}

void Notifier::Shutdown() {
  std::vector<X_HANDLE> listeners;
  {
    std::lock_guard lock(mutex_);
    listeners.swap(listeners_);
  }
  for (X_HANDLE handle : listeners) {
    CloseOwned(table_, handle);
  }
}

}  // namespace rex::runtime
