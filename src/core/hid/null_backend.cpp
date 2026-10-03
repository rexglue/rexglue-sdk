#include <rex/hid/backend.h>

namespace rex::hid {

namespace {

rex::Error NotConnected() {
  return MakeError(Error::NotConnected, "only user 0 has the idle pad");
}

}  // namespace

Result<X_INPUT_CAPABILITIES> NullBackend::GetCapabilities(u32 user, u32) {
  if (user != 0) {
    return std::unexpected(NotConnected());
  }
  X_INPUT_CAPABILITIES caps{};
  caps.type = XINPUT_DEVTYPE_GAMEPAD;
  caps.sub_type = XINPUT_DEVSUBTYPE_GAMEPAD;
  caps.gamepad.buttons = 0xFFFF;
  caps.gamepad.left_trigger = 0xFF;
  caps.gamepad.right_trigger = 0xFF;
  caps.gamepad.thumb_lx = static_cast<i16>(0x7FFF);
  caps.gamepad.thumb_ly = static_cast<i16>(0x7FFF);
  caps.gamepad.thumb_rx = static_cast<i16>(0x7FFF);
  caps.gamepad.thumb_ry = static_cast<i16>(0x7FFF);
  caps.vibration.left_motor_speed = 0xFFFF;
  caps.vibration.right_motor_speed = 0xFFFF;
  return caps;
}

Result<X_INPUT_STATE> NullBackend::GetState(u32 user) {
  if (user != 0) {
    return std::unexpected(NotConnected());
  }
  std::lock_guard lock(mutex_);
  if (!reported_) {
    reported_ = true;
    if (listener_) {
      listener_(0, true);
    }
  }
  return X_INPUT_STATE{};
}

Result<void> NullBackend::SetVibration(u32 user, const X_INPUT_VIBRATION&) {
  if (user != 0) {
    return std::unexpected(NotConnected());
  }
  return Ok();
}

Result<X_INPUT_KEYSTROKE> NullBackend::GetKeystroke(u32 user, u32) {
  if (user != 0) {
    return std::unexpected(NotConnected());
  }
  return std::unexpected(MakeError(Error::Empty, "the idle pad has no keystrokes"));
}

void NullBackend::SetConnectionListener(std::move_only_function<void(u32, bool)> listener) {
  std::lock_guard lock(mutex_);
  listener_ = std::move(listener);
}

}  // namespace rex::hid
