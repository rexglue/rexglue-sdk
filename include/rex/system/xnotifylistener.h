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
#include <unordered_map>

#include <rex/assert.h>
#include <rex/system/xcontent.h>
#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/thread.h>
#include <rex/thread/mutex.h>

namespace rex::system {

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
    static_assert_size(*this, sizeof(id));
  }

  constexpr operator XNotificationID() { return id; }
};

class XNotifyListener : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::NotifyListener;

  explicit XNotifyListener(KernelState* kernel_state);
  ~XNotifyListener() override;

  uint64_t mask() const { return mask_; }
  uint32_t max_version() const { return max_version_; }

  void Initialize(uint64_t mask, uint32_t max_version);

  void EnqueueNotification(XNotificationID id, uint32_t data);
  bool DequeueNotification(XNotificationID* out_id, uint32_t* out_data);
  bool DequeueNotification(XNotificationID id, uint32_t* out_data);

  bool Save(stream::ByteStream* stream) override;
  static object_ref<XNotifyListener> Restore(KernelState* kernel_state, stream::ByteStream* stream);

 protected:
  rex::thread::WaitHandle* GetWaitHandle() override { return wait_handle_.get(); }

 private:
  std::unique_ptr<rex::thread::Event> wait_handle_;
  rex::thread::global_critical_region global_critical_region_;
  std::vector<std::pair<XNotificationID, uint32_t>> notifications_;
  uint64_t mask_ = 0;
  uint32_t max_version_ = 0;
};

}  // namespace rex::system
