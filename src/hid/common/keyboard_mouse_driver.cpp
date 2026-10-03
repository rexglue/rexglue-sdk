#include "hid/common/keyboard_mouse_driver.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>

#include <rex/bit.h>
#include <rex/cvar.h>
#include <rex/hid/virtual_key.h>

#include "hid/common/cvar_table.h"
#include "hid/common/hub.h"

namespace rex::hid {

namespace {

// A single device, so its handle is a constant.
constexpr DeviceId kKeyboardMouseDevice = static_cast<DeviceId>(0x4D4E4B00);

// Bounds the queue for titles that never call XamInputGetKeystroke.
constexpr size_t kMaxQueuedKeystrokes = 256;

// Bind values are a comma-separated list of alternatives, each optionally
// carrying modifier prefixes: "Q,I" or "Shift+W". Modifier matching is exact,
// so "Up" stays silent while Shift is held and "Shift+Up" stays silent without
// it. That is what lets the D-pad share the arrow keys. A consequence is
// that binding a bare modifier name ("Shift") can never fire, since holding it
// makes the live mask non-zero while the bind wants zero.
constexpr u8 kModShift = 1u << 0;
constexpr u8 kModCtrl = 1u << 1;
constexpr u8 kModAlt = 1u << 2;

u8 LiveModifiers(const bool (&key_down)[256]) {
  u8 mods = 0;
  if (key_down[static_cast<u16>(VirtualKey::kShift)])
    mods |= kModShift;
  if (key_down[static_cast<u16>(VirtualKey::kControl)])
    mods |= kModCtrl;
  if (key_down[static_cast<u16>(VirtualKey::kMenu)])
    mods |= kModAlt;
  return mods;
}

// Strips leading modifier prefixes off 'token', advancing it to the bare key
// name and returning the mask they require.
u8 TakeModifiers(std::string_view& token) {
  u8 mods = 0;
  for (;;) {
    size_t plus = token.find('+');
    if (plus == std::string_view::npos || plus == 0) {
      break;
    }
    std::string_view head = token.substr(0, plus);
    u8 bit = 0;
    if (head == "Shift") {
      bit = kModShift;
    } else if (head == "Ctrl" || head == "Control") {
      bit = kModCtrl;
    } else if (head == "Alt") {
      bit = kModAlt;
    } else {
      break;
    }
    mods |= bit;
    token.remove_prefix(plus + 1);
  }
  return mods;
}

std::string_view TrimSpaces(std::string_view s) {
  while (!s.empty() && s.front() == ' ') {
    s.remove_prefix(1);
  }
  while (!s.empty() && s.back() == ' ') {
    s.remove_suffix(1);
  }
  return s;
}

bool TokenPressed(const bool (&key_down)[256], std::string_view token, u8 live_mods) {
  u8 want = TakeModifiers(token);
  if (want != live_mods) {
    return false;
  }
  VirtualKey vk = ParseVirtualKey(token);
  if (vk == VirtualKey::kNone) {
    return false;
  }
  u16 idx = static_cast<u16>(vk);
  return idx < 256 && key_down[idx];
}

// hid::VirtualKey follows Windows VK_ numbering; the title wants USB HID usage
// page 0x07.
u8 VirtualKeyToHIDUsage(VirtualKey vk_enum) {
  const u32 vk = static_cast<u32>(vk_enum);

  // The runs below are contiguous in both numbering schemes.
  if (vk >= 'A' && vk <= 'Z') {
    return static_cast<u8>(vk - 'A' + 0x04);
  }
  // 0 is irregular in both digit runs; handled in the switch.
  if (vk >= '1' && vk <= '9') {
    return static_cast<u8>(vk - '1' + 0x1E);
  }
  if (vk >= static_cast<u32>(VirtualKey::kF1) && vk <= static_cast<u32>(VirtualKey::kF12)) {
    return static_cast<u8>(vk - static_cast<u32>(VirtualKey::kF1) + 0x3A);
  }
  if (vk >= static_cast<u32>(VirtualKey::kF13) && vk <= static_cast<u32>(VirtualKey::kF24)) {
    return static_cast<u8>(vk - static_cast<u32>(VirtualKey::kF13) + 0x68);
  }
  if (vk >= static_cast<u32>(VirtualKey::kNumpad1) &&
      vk <= static_cast<u32>(VirtualKey::kNumpad9)) {
    return static_cast<u8>(vk - static_cast<u32>(VirtualKey::kNumpad1) + 0x59);
  }
  if (vk >= static_cast<u32>(VirtualKey::kLShift) && vk <= static_cast<u32>(VirtualKey::kLMenu)) {
    return static_cast<u8>(vk - static_cast<u32>(VirtualKey::kLShift) + 0xE1);
  }

  switch (vk_enum) {
    case VirtualKey::k0:
      return 0x27;
    case VirtualKey::kReturn:
      return 0x28;
    case VirtualKey::kEscape:
      return 0x29;
    case VirtualKey::kBack:
      return 0x2A;
    case VirtualKey::kTab:
      return 0x2B;
    case VirtualKey::kSpace:
      return 0x2C;
    case VirtualKey::kOemMinus:
      return 0x2D;
    case VirtualKey::kOemPlus:
      return 0x2E;
    case VirtualKey::kOem4:
      return 0x2F;
    case VirtualKey::kOem6:
      return 0x30;
    case VirtualKey::kOem5:
      return 0x31;
    case VirtualKey::kOem1:
      return 0x33;
    case VirtualKey::kOem7:
      return 0x34;
    case VirtualKey::kOem3:
      return 0x35;
    case VirtualKey::kOemComma:
      return 0x36;
    case VirtualKey::kOemPeriod:
      return 0x37;
    case VirtualKey::kOem2:
      return 0x38;
    case VirtualKey::kCapital:
      return 0x39;
    case VirtualKey::kSnapshot:
      return 0x46;
    case VirtualKey::kScroll:
      return 0x47;
    case VirtualKey::kPause:
      return 0x48;
    case VirtualKey::kInsert:
      return 0x49;
    case VirtualKey::kHome:
      return 0x4A;
    case VirtualKey::kPrior:
      return 0x4B;
    case VirtualKey::kDelete:
      return 0x4C;
    case VirtualKey::kEnd:
      return 0x4D;
    case VirtualKey::kNext:
      return 0x4E;
    case VirtualKey::kRight:
      return 0x4F;
    case VirtualKey::kLeft:
      return 0x50;
    case VirtualKey::kDown:
      return 0x51;
    case VirtualKey::kUp:
      return 0x52;
    case VirtualKey::kNumLock:
      return 0x53;
    case VirtualKey::kDivide:
      return 0x54;
    case VirtualKey::kMultiply:
      return 0x55;
    case VirtualKey::kSubtract:
      return 0x56;
    case VirtualKey::kAdd:
      return 0x57;
    case VirtualKey::kNumpad0:
      return 0x62;
    case VirtualKey::kDecimal:
      return 0x63;
    case VirtualKey::kApps:
      return 0x65;
    default:
      return 0x00;
  }
}

// Sticks and triggers are included: XInput reports those as directional pad
// keys, which is what menu navigation reads.
struct PadBind {
  std::string keys;
  VirtualKey pad_key;
};

// Rebuilt per call because the cvars are live-editable.
std::array<PadBind, 24> PadBinds() {
  return {{
      {REXCVAR_GET(cvars::keybind_a), VirtualKey::kXInputPadA},
      {REXCVAR_GET(cvars::keybind_b), VirtualKey::kXInputPadB},
      {REXCVAR_GET(cvars::keybind_x), VirtualKey::kXInputPadX},
      {REXCVAR_GET(cvars::keybind_y), VirtualKey::kXInputPadY},
      {REXCVAR_GET(cvars::keybind_left_shoulder), VirtualKey::kXInputPadLShoulder},
      {REXCVAR_GET(cvars::keybind_right_shoulder), VirtualKey::kXInputPadRShoulder},
      {REXCVAR_GET(cvars::keybind_left_trigger), VirtualKey::kXInputPadLTrigger},
      {REXCVAR_GET(cvars::keybind_right_trigger), VirtualKey::kXInputPadRTrigger},
      {REXCVAR_GET(cvars::keybind_lstick_press), VirtualKey::kXInputPadLThumbPress},
      {REXCVAR_GET(cvars::keybind_rstick_press), VirtualKey::kXInputPadRThumbPress},
      {REXCVAR_GET(cvars::keybind_back), VirtualKey::kXInputPadBack},
      {REXCVAR_GET(cvars::keybind_start), VirtualKey::kXInputPadStart},
      {REXCVAR_GET(cvars::keybind_dpad_up), VirtualKey::kXInputPadDpadUp},
      {REXCVAR_GET(cvars::keybind_dpad_down), VirtualKey::kXInputPadDpadDown},
      {REXCVAR_GET(cvars::keybind_dpad_left), VirtualKey::kXInputPadDpadLeft},
      {REXCVAR_GET(cvars::keybind_dpad_right), VirtualKey::kXInputPadDpadRight},
      {REXCVAR_GET(cvars::keybind_lstick_up), VirtualKey::kXInputPadLThumbUp},
      {REXCVAR_GET(cvars::keybind_lstick_down), VirtualKey::kXInputPadLThumbDown},
      {REXCVAR_GET(cvars::keybind_lstick_left), VirtualKey::kXInputPadLThumbLeft},
      {REXCVAR_GET(cvars::keybind_lstick_right), VirtualKey::kXInputPadLThumbRight},
      {REXCVAR_GET(cvars::keybind_rstick_up), VirtualKey::kXInputPadRThumbUp},
      {REXCVAR_GET(cvars::keybind_rstick_down), VirtualKey::kXInputPadRThumbDown},
      {REXCVAR_GET(cvars::keybind_rstick_left), VirtualKey::kXInputPadRThumbLeft},
      {REXCVAR_GET(cvars::keybind_rstick_right), VirtualKey::kXInputPadRThumbRight},
  }};
}

bool IsPassthroughEnabled() {
  return REXCVAR_GET(cvars::mnk_passthrough);
}

bool IsBindPressed(const bool (&key_down)[256], const std::string& cvar_val) {
  const u8 live_mods = LiveModifiers(key_down);
  std::string_view rest(cvar_val);
  while (!rest.empty()) {
    size_t comma = rest.find(',');
    std::string_view token = rest.substr(0, comma);
    if (comma == std::string_view::npos) {
      rest = std::string_view();
    } else {
      rest.remove_prefix(comma + 1);
    }
    token = TrimSpaces(token);
    if (!token.empty() && TokenPressed(key_down, token, live_mods)) {
      return true;
    }
  }
  return false;
}

}  // namespace

KeyboardMouseDriver::KeyboardMouseDriver(Hub& hub) : hub_(hub) {}

bool KeyboardMouseDriver::IsEnabled() const {
  // Passthrough enables the device on its own.
  return REXCVAR_GET(cvars::mnk_mode) || REXCVAR_GET(cvars::mnk_passthrough);
}

void KeyboardMouseDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  // Disabled means no device at all, so it never occupies a title user slot.
  if (!IsEnabled()) {
    return;
  }
  DeviceInfo info;
  info.id = kKeyboardMouseDevice;
  info.name = IsPassthroughEnabled() ? "Keyboard" : "Keyboard and Mouse";
  info.subtype =
      IsPassthroughEnabled() ? XINPUT_DEVSUBTYPE_USB_KEYBOARD : XINPUT_DEVSUBTYPE_GAMEPAD;
  info.synthetic = true;
  out.push_back(info);
}

Result<X_INPUT_CAPABILITIES> KeyboardMouseDriver::GetDeviceCapabilities(DeviceId id, u32) {
  if (!IsEnabled() || id != kKeyboardMouseDevice) {
    return std::unexpected(MakeError(Error::NotConnected, "keyboard and mouse emulation is off"));
  }
  X_INPUT_CAPABILITIES caps{};
  if (IsPassthroughEnabled()) {
    caps.type = XINPUT_DEVTYPE_KEYBOARD;
    caps.sub_type = XINPUT_DEVSUBTYPE_USB_KEYBOARD;
    return caps;
  }
  caps.type = XINPUT_DEVTYPE_GAMEPAD;
  caps.sub_type = XINPUT_DEVSUBTYPE_GAMEPAD;
  caps.flags = 0;
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

Result<X_INPUT_STATE> KeyboardMouseDriver::GetDeviceState(DeviceId id) {
  if (!IsEnabled() || id != kKeyboardMouseDevice) {
    return std::unexpected(MakeError(Error::NotConnected, "keyboard and mouse emulation is off"));
  }

  // Keys reach the title through GetDeviceKeystroke only, leaving the title
  // user free for a real controller.
  if (IsPassthroughEnabled()) {
    return std::unexpected(
        MakeError(Error::NotConnected, "passthrough keeps the user free for a pad"));
  }

  // Mouse look is opt in. Without this gate, keyboard input alone would hide
  // and lock the cursor, breaking the ImGui overlays.
  const bool lock = REXCVAR_GET(cvars::mnk_mouse) && hub_.focused() && is_active();
  if (pointer_lock_.exchange(lock, std::memory_order_relaxed) != lock) {
    hub_.RefreshPointerMode();
  }

  X_INPUT_STATE state{};
  if (!is_active() || !hub_.focused()) {
    state.packet_number = packet_number_;
    return state;
  }

  std::lock_guard guard(state_mutex_);

  u16 buttons = 0;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_a)))
    buttons |= X_INPUT_GAMEPAD_A;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_b)))
    buttons |= X_INPUT_GAMEPAD_B;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_x)))
    buttons |= X_INPUT_GAMEPAD_X;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_y)))
    buttons |= X_INPUT_GAMEPAD_Y;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_left_shoulder)))
    buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_right_shoulder)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_lstick_press)))
    buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_rstick_press)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_back)))
    buttons |= X_INPUT_GAMEPAD_BACK;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_start)))
    buttons |= X_INPUT_GAMEPAD_START;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_guide)))
    buttons |= X_INPUT_GAMEPAD_GUIDE;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_dpad_up)))
    buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_dpad_down)))
    buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_dpad_left)))
    buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_dpad_right)))
    buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;

  u8 lt = IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_left_trigger)) ? 0xFF : 0;
  u8 rt = IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_right_trigger)) ? 0xFF : 0;

  i32 lx = 0;
  i32 ly = 0;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_lstick_left)))
    lx -= INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_lstick_right)))
    lx += INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_lstick_up)))
    ly += INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_lstick_down)))
    ly -= INT16_MAX;

  i32 rx = 0;
  i32 ry = 0;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_rstick_left)))
    rx -= INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_rstick_right)))
    rx += INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_rstick_up)))
    ry += INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(cvars::keybind_rstick_down)))
    ry -= INT16_MAX;

  if (REXCVAR_GET(cvars::mnk_mouse)) {
    f64 sensitivity = REXCVAR_GET(cvars::mnk_sensitivity);
    constexpr f64 kBaseScale = 200.0;
    rx += static_cast<i32>(static_cast<f64>(mouse_dx_) * sensitivity * kBaseScale);
    ry += static_cast<i32>(static_cast<f64>(-mouse_dy_) * sensitivity * kBaseScale);
  }
  // Drained unconditionally: deltas keep accumulating in OnMouseMove while the
  // mouse is off, and toggling it on would otherwise dump the whole backlog
  // into one frame as a camera snap.
  mouse_dx_ = 0.0f;
  mouse_dy_ = 0.0f;

  auto clamp16 = [](i32 v) -> i16 {
    return static_cast<i16>(
        std::clamp(v, static_cast<i32>(INT16_MIN), static_cast<i32>(INT16_MAX)));
  };

  packet_number_++;

  state.packet_number = packet_number_;
  state.gamepad.buttons = buttons;
  state.gamepad.left_trigger = lt;
  state.gamepad.right_trigger = rt;
  state.gamepad.thumb_lx = clamp16(lx);
  state.gamepad.thumb_ly = clamp16(ly);
  state.gamepad.thumb_rx = clamp16(rx);
  state.gamepad.thumb_ry = clamp16(ry);
  return state;
}

Result<void> KeyboardMouseDriver::SetDeviceVibration(DeviceId id, const X_INPUT_VIBRATION&) {
  if (!IsEnabled() || id != kKeyboardMouseDevice) {
    return std::unexpected(MakeError(Error::NotConnected, "keyboard and mouse emulation is off"));
  }
  return Ok();
}

Result<X_INPUT_KEYSTROKE> KeyboardMouseDriver::GetDeviceKeystroke(DeviceId id, u32) {
  if (!IsEnabled() || id != kKeyboardMouseDevice) {
    return std::unexpected(MakeError(Error::NotConnected, "keyboard and mouse emulation is off"));
  }
  std::lock_guard lock(state_mutex_);
  if (keystroke_queue_.empty()) {
    return std::unexpected(MakeError(Error::Empty, "no keystroke is queued"));
  }
  X_INPUT_KEYSTROKE keystroke = keystroke_queue_.front();
  keystroke_queue_.pop();
  return keystroke;
}

void KeyboardMouseDriver::OnEvent(const InputEvent& event) {
  switch (event.kind) {
    case InputEvent::Kind::KeyDown:
      OnKey(event, true);
      break;
    case InputEvent::Kind::KeyUp:
      OnKey(event, false);
      break;
    case InputEvent::Kind::KeyChar:
      OnChar(event);
      break;
    case InputEvent::Kind::ButtonDown:
      OnButton(event, true);
      break;
    case InputEvent::Kind::ButtonUp:
      OnButton(event, false);
      break;
    case InputEvent::Kind::Motion:
      OnMotion(event);
      break;
    case InputEvent::Kind::FocusLost:
      OnFocusLost();
      break;
    default:
      break;
  }
}

void KeyboardMouseDriver::EnqueueKeystroke(u16 pad_key, bool down) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = pad_key;
  ks.unicode = 0;
  ks.flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  // The hub stamps the title user this device is assigned to.
  ks.user_index = 0;
  ks.hid_code = 0;
  PushKeystroke(ks);
}

void KeyboardMouseDriver::PushKeystroke(const X_INPUT_KEYSTROKE& keystroke) {
  if (keystroke_queue_.size() >= kMaxQueuedKeystrokes) {
    keystroke_queue_.pop();
  }
  keystroke_queue_.push(keystroke);
}

void KeyboardMouseDriver::EnqueueRawKeystroke(const InputEvent& event, bool down) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = static_cast<u16>(event.key);
  ks.unicode = 0;
  ks.user_index = 0;
  ks.hid_code = VirtualKeyToHIDUsage(event.key);

  u16 flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  // Already down, so this is the OS auto-repeat.
  if (down && event.repeat) {
    flags |= X_INPUT_KEYSTROKE_REPEAT;
  }
  if (HasFlags(event.modifiers, KeyModifiers::Shift)) {
    flags |= X_INPUT_KEYSTROKE_SHIFT;
  }
  if (HasFlags(event.modifiers, KeyModifiers::Ctrl)) {
    flags |= X_INPUT_KEYSTROKE_CTRL;
  }
  if (HasFlags(event.modifiers, KeyModifiers::Alt)) {
    flags |= X_INPUT_KEYSTROKE_ALT;
  }
  ks.flags = flags;
  PushKeystroke(ks);
}

void KeyboardMouseDriver::RefreshBoundKeystrokesLocked() {
  const auto binds = PadBinds();
  u32 pressed = 0;
  for (size_t i = 0; i < binds.size(); i++) {
    if (IsBindPressed(key_down_, binds[i].keys)) {
      pressed |= u32{1} << i;
    }
  }

  // The whole table is diffed rather than the key that moved: binds carry
  // modifiers, so Shift alone can flip several of them at once.
  const u32 changed = pressed ^ bound_pressed_;
  bound_pressed_ = pressed;
  if (!changed) {
    return;
  }
  for (size_t i = 0; i < binds.size(); i++) {
    const u32 bit = u32{1} << i;
    if (changed & bit) {
      EnqueueKeystroke(static_cast<u16>(binds[i].pad_key), (pressed & bit) != 0);
    }
  }
}

void KeyboardMouseDriver::SetKeyState(u16 key, bool down) {
  if (key < 256) {
    key_down_[key] = down;
  }
}

void KeyboardMouseDriver::OnKey(const InputEvent& event, bool down) {
  if (!IsEnabled() || (down && !hub_.focused())) {
    return;
  }
  std::lock_guard lock(state_mutex_);
  if (IsPassthroughEnabled()) {
    EnqueueRawKeystroke(event, down);
    return;
  }
  SetKeyState(static_cast<u16>(event.key), down);
  RefreshBoundKeystrokesLocked();
}

void KeyboardMouseDriver::OnChar(const InputEvent& event) {
  if (!IsEnabled() || !IsPassthroughEnabled()) {
    return;
  }
  // WM_CHAR convention: the codepoint arrives after its key-down, so it is
  // attached to the keystroke already queued for that key.
  std::lock_guard lock(state_mutex_);
  if (keystroke_queue_.empty()) {
    return;
  }
  X_INPUT_KEYSTROKE& last = keystroke_queue_.back();
  const u16 flags = last.flags;
  if (!(flags & X_INPUT_KEYSTROKE_KEYDOWN) || last.unicode != 0) {
    return;
  }
  last.unicode = static_cast<u16>(event.character);
  last.flags = static_cast<u16>(flags | X_INPUT_KEYSTROKE_VALIDUNICODE);
}

void KeyboardMouseDriver::OnButton(const InputEvent& event, bool down) {
  if (!IsEnabled() || (down && !hub_.focused())) {
    return;
  }
  std::lock_guard lock(state_mutex_);
  switch (event.button) {
    case MouseButton::Left:
      SetKeyState(static_cast<u16>(VirtualKey::kLButton), down);
      break;
    case MouseButton::Right:
      SetKeyState(static_cast<u16>(VirtualKey::kRButton), down);
      break;
    case MouseButton::Middle:
      SetKeyState(static_cast<u16>(VirtualKey::kMButton), down);
      break;
    default:
      break;
  }
  RefreshBoundKeystrokesLocked();
}

void KeyboardMouseDriver::OnMotion(const InputEvent& event) {
  if (!IsEnabled() || !hub_.focused()) {
    return;
  }
  std::lock_guard lock(state_mutex_);
  mouse_dx_ += event.dx;
  mouse_dy_ += event.dy;
}

void KeyboardMouseDriver::OnFocusLost() {
  pointer_lock_.store(false, std::memory_order_relaxed);
  std::lock_guard lock(state_mutex_);
  std::memset(key_down_, 0, sizeof(key_down_));
  // Releases landing on another window never arrive here as key-ups.
  RefreshBoundKeystrokesLocked();
  mouse_dx_ = 0.0f;
  mouse_dy_ = 0.0f;
}

}  // namespace rex::hid
