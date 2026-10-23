#include "core/hid/idle_pad_caps.h"

namespace rex::hid::detail {

X_INPUT_CAPABILITIES MakeIdlePadCapabilities() {
  X_INPUT_CAPABILITIES caps{};
  caps.type = kXInputDevTypeGamepad;
  caps.sub_type = kXInputDevSubtypeGamepad;
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

}  // namespace rex::hid::detail
