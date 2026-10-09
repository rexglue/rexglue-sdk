#include "hid/common/state_merge.h"

#include <algorithm>
#include <cstdlib>

namespace rex::hid {

namespace {

i16 LargerMagnitude(i16 a, i16 b) {
  return std::abs(static_cast<int>(a)) >= std::abs(static_cast<int>(b)) ? a : b;
}

bool PastDeadzone(i16 axis) {
  return std::abs(static_cast<int>(axis)) >= kThumbDeadzone;
}

}  // namespace

void MergeInto(X_INPUT_STATE& dst, const X_INPUT_STATE& src) {
  dst.gamepad.buttons =
      static_cast<u16>(dst.gamepad.buttons) | static_cast<u16>(src.gamepad.buttons);
  dst.gamepad.left_trigger = std::max(dst.gamepad.left_trigger, src.gamepad.left_trigger);
  dst.gamepad.right_trigger = std::max(dst.gamepad.right_trigger, src.gamepad.right_trigger);
  dst.gamepad.thumb_lx = LargerMagnitude(dst.gamepad.thumb_lx, src.gamepad.thumb_lx);
  dst.gamepad.thumb_ly = LargerMagnitude(dst.gamepad.thumb_ly, src.gamepad.thumb_ly);
  dst.gamepad.thumb_rx = LargerMagnitude(dst.gamepad.thumb_rx, src.gamepad.thumb_rx);
  dst.gamepad.thumb_ry = LargerMagnitude(dst.gamepad.thumb_ry, src.gamepad.thumb_ry);

  if (static_cast<u32>(src.packet_number) > static_cast<u32>(dst.packet_number)) {
    dst.packet_number = src.packet_number;
  }
}

bool IsNeutral(const X_INPUT_GAMEPAD& gamepad) {
  if (static_cast<u16>(gamepad.buttons) != 0) {
    return false;
  }
  if (gamepad.left_trigger >= kTriggerThreshold || gamepad.right_trigger >= kTriggerThreshold) {
    return false;
  }
  return !PastDeadzone(gamepad.thumb_lx) && !PastDeadzone(gamepad.thumb_ly) &&
         !PastDeadzone(gamepad.thumb_rx) && !PastDeadzone(gamepad.thumb_ry);
}

void ActiveDeviceTracker::Observe(u32 user_index, DeviceId id, const X_INPUT_GAMEPAD& gamepad) {
  if (user_index >= kMaxGuestUsers || IsNeutral(gamepad)) {
    return;
  }
  active_[user_index] = id;
}

DeviceId ActiveDeviceTracker::Active(u32 user_index) const {
  if (user_index >= kMaxGuestUsers) {
    return DeviceId::kInvalid;
  }
  return active_[user_index];
}

void ActiveDeviceTracker::Forget(DeviceId id) {
  for (auto& active : active_) {
    if (active == id) {
      active = DeviceId::kInvalid;
    }
  }
}

}  // namespace rex::hid
