#include "hid/keyboard_mouse_driver.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <ranges>
#include <string>
#include <string_view>

#include <rex/string.h>
#include <rex/bit.h>
#include <rex/cvar.h>
#include <rex/hid/key_chord.h>
#include <rex/hid/virtual_key.h>
#include <rex/logging/log.h>

#include "hid/cvar_table.h"
#include "hid/hub.h"

namespace rex::hid {

namespace {

constexpr DeviceId kKeyboardMouseDevice = static_cast<DeviceId>(0x4D4E4B00);

/** Queue bound for titles that never call XamInputGetKeystroke. */
constexpr size_t kMaxQueuedKeystrokes = 256;

KeyModifiers HeldModifiers(const bool (&key_down)[256]) {
  KeyModifiers mods = KeyModifiers::kNone;
  if (key_down[std::to_underlying(VirtualKey::kShift)])
    mods |= KeyModifiers::kShift;
  if (key_down[std::to_underlying(VirtualKey::kControl)])
    mods |= KeyModifiers::kCtrl;
  if (key_down[std::to_underlying(VirtualKey::kMenu)])
    mods |= KeyModifiers::kAlt;
  return mods;
}

bool IsBindPressed(const bool (&key_down)[256], const std::vector<KeyChord>& chords) {
  const KeyModifiers live = HeldModifiers(key_down);
  return std::ranges::any_of(chords, [&](const KeyChord& chord) {
    const u16 index = static_cast<u16>(chord.key);
    return chord.modifiers == live && index < 256 && key_down[index];
  });
}

/**
 * Maps a virtual key to a USB HID usage on page 0x07. Letters, digits 1 to 9 and F1 to F12
 * are contiguous runs in both numberings. Digit 0 is irregular and handled in the switch.
 *
 * @param vk_enum  Virtual key following Windows VK_ numbering.
 * @return HID usage, 0 when unmapped.
 */
u8 VirtualKeyToHIDUsage(VirtualKey vk_enum) {
  const u32 vk = static_cast<u32>(vk_enum);

  if (vk >= 'A' && vk <= 'Z') {
    return static_cast<u8>(vk - 'A' + 0x04);
  }
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

enum BindIndex : u8 {
  kBindA,
  kBindB,
  kBindX,
  kBindY,
  kBindLeftShoulder,
  kBindRightShoulder,
  kBindLeftTrigger,
  kBindRightTrigger,
  kBindLStickPress,
  kBindRStickPress,
  kBindBack,
  kBindStart,
  kBindDPadUp,
  kBindDPadDown,
  kBindDPadLeft,
  kBindDPadRight,
  kBindLStickUp,
  kBindLStickDown,
  kBindLStickLeft,
  kBindLStickRight,
  kBindRStickUp,
  kBindRStickDown,
  kBindRStickLeft,
  kBindRStickRight,
  kBindGuide,
};

struct BindSpec {
  const cvar::Var<std::string>* var;
  VirtualKey pad_key;
};

const std::array<BindSpec, KeyboardMouseDriver::kBindCount> kBindSpecs = {{
    {&cvars::keybind_a, VirtualKey::kXInputPadA},
    {&cvars::keybind_b, VirtualKey::kXInputPadB},
    {&cvars::keybind_x, VirtualKey::kXInputPadX},
    {&cvars::keybind_y, VirtualKey::kXInputPadY},
    {&cvars::keybind_left_shoulder, VirtualKey::kXInputPadLShoulder},
    {&cvars::keybind_right_shoulder, VirtualKey::kXInputPadRShoulder},
    {&cvars::keybind_left_trigger, VirtualKey::kXInputPadLTrigger},
    {&cvars::keybind_right_trigger, VirtualKey::kXInputPadRTrigger},
    {&cvars::keybind_lstick_press, VirtualKey::kXInputPadLThumbPress},
    {&cvars::keybind_rstick_press, VirtualKey::kXInputPadRThumbPress},
    {&cvars::keybind_back, VirtualKey::kXInputPadBack},
    {&cvars::keybind_start, VirtualKey::kXInputPadStart},
    {&cvars::keybind_dpad_up, VirtualKey::kXInputPadDPadUp},
    {&cvars::keybind_dpad_down, VirtualKey::kXInputPadDpadDown},
    {&cvars::keybind_dpad_left, VirtualKey::kXInputPadDpadLeft},
    {&cvars::keybind_dpad_right, VirtualKey::kXInputPadDpadRight},
    {&cvars::keybind_lstick_up, VirtualKey::kXInputPadLThumbUp},
    {&cvars::keybind_lstick_down, VirtualKey::kXInputPadLThumbDown},
    {&cvars::keybind_lstick_left, VirtualKey::kXInputPadLThumbLeft},
    {&cvars::keybind_lstick_right, VirtualKey::kXInputPadLThumbRight},
    {&cvars::keybind_rstick_up, VirtualKey::kXInputPadRThumbUp},
    {&cvars::keybind_rstick_down, VirtualKey::kXInputPadRThumbDown},
    {&cvars::keybind_rstick_left, VirtualKey::kXInputPadRThumbLeft},
    {&cvars::keybind_rstick_right, VirtualKey::kXInputPadRThumbRight},
    {&cvars::keybind_guide, VirtualKey::kNone},
}};

static_assert(kBindGuide + 1 == KeyboardMouseDriver::kBindCount);

bool IsPassthroughEnabled() {
  return REXCVAR_GET(cvars::mnk_passthrough);
}

}  // namespace

KeyboardMouseDriver::KeyboardMouseDriver(Hub& hub) : hub_(hub) {}

void KeyboardMouseDriver::Attach(cvar::ICvarRegistry& cvars) {
  Detach();
  for (size_t i = 0; i < kBindSpecs.size(); i++) {
    const cvar::Var<std::string>& var = *kBindSpecs[i].var;
    SetBind(i, var.Get());
    auto handle =
        cvars.OnChange(var.GetDesc().name,
                       [this, i](std::string_view, std::string_view value) { SetBind(i, value); });
    if (!handle) {
      REXLOG_WARN(hid, "{} is not followed, {}", var.GetDesc().name, handle.error().message);
      continue;
    }
    handles_[i] = std::move(*handle);
  }
}

void KeyboardMouseDriver::Detach() {
  for (auto& handle : handles_) {
    handle.Reset();
  }
  std::lock_guard lock(state_mutex_);
  for (auto& chords : binds_) {
    chords.clear();
  }
}

void KeyboardMouseDriver::SetBind(size_t index, std::string_view text) {
  const std::string_view name = kBindSpecs[index].var->GetDesc().name;
  std::vector<KeyChord> chords;
  bool any_item = false;
  for (const auto part : std::views::split(text, ',')) {
    const std::string_view item = Trim(std::string_view(part.begin(), part.end()));
    if (item.empty()) {
      continue;
    }
    any_item = true;
    auto chord = ParseKeyChord(item);
    if (!chord) {
      REXLOG_WARN(hid, "{} skips an item, {}", name, chord.error().message);
      continue;
    }
    chords.push_back(*chord);
  }
  if (any_item && chords.empty()) {
    REXLOG_WARN(hid, "{} keeps its previous keys, no item of '{}' names a key", name, text);
    return;
  }
  std::lock_guard lock(state_mutex_);
  binds_[index] = std::move(chords);
}

bool KeyboardMouseDriver::IsEnabled() const {
  return REXCVAR_GET(cvars::mnk_mode) || REXCVAR_GET(cvars::mnk_passthrough);
}

void KeyboardMouseDriver::EnumerateDevices(std::vector<Device>& out) {
  if (!IsEnabled()) {
    return;
  }
  Device info;
  info.id = kKeyboardMouseDevice;
  info.name = IsPassthroughEnabled() ? "Keyboard" : "Keyboard and Mouse";
  info.subtype = IsPassthroughEnabled() ? kXInputDevSubtypeUSBKeyboard : kXInputDevSubtypeGamepad;
  info.synthetic = true;
  out.push_back(info);
}

Result<X_INPUT_CAPABILITIES> KeyboardMouseDriver::GetDeviceCapabilities(DeviceId id, u32) {
  if (!IsEnabled() || id != kKeyboardMouseDevice) {
    return std::unexpected(
        BuildError(HIDError::kNotConnected, "keyboard and mouse emulation is off"));
  }
  X_INPUT_CAPABILITIES caps{};
  if (IsPassthroughEnabled()) {
    caps.type = kXInputDevTypeKeyboard;
    caps.sub_type = kXInputDevSubtypeUSBKeyboard;
    return caps;
  }
  caps.type = kXInputDevTypeGamepad;
  caps.sub_type = kXInputDevSubtypeGamepad;
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
    return std::unexpected(
        BuildError(HIDError::kNotConnected, "keyboard and mouse emulation is off"));
  }

  // Passthrough delivers keys through GetDeviceKeystroke only, leaving the title user free.
  if (IsPassthroughEnabled()) {
    return std::unexpected(
        BuildError(HIDError::kNotConnected, "passthrough keeps the user free for a pad"));
  }

  // Mouse look is opt in, else keyboard input alone would hide and lock the cursor over ImGui.
  const bool lock = REXCVAR_GET(cvars::mnk_mouse) && hub_.IsFocused() && IsActive();
  if (pointer_lock_.exchange(lock, std::memory_order_relaxed) != lock) {
    hub_.RefreshPointerMode();
  }

  X_INPUT_STATE state{};
  if (!IsActive() || !hub_.IsFocused()) {
    state.packet_number = packet_number_;
    return state;
  }

  std::lock_guard guard(state_mutex_);

  u16 buttons = 0;
  if (IsBindPressed(key_down_, binds_[kBindA]))
    buttons |= kXInputGamepadA;
  if (IsBindPressed(key_down_, binds_[kBindB]))
    buttons |= kXInputGamepadB;
  if (IsBindPressed(key_down_, binds_[kBindX]))
    buttons |= kXInputGamepadX;
  if (IsBindPressed(key_down_, binds_[kBindY]))
    buttons |= kXInputGamepadY;
  if (IsBindPressed(key_down_, binds_[kBindLeftShoulder]))
    buttons |= kXInputGamepadLeftShoulder;
  if (IsBindPressed(key_down_, binds_[kBindRightShoulder]))
    buttons |= kXInputGamepadRightShoulder;
  if (IsBindPressed(key_down_, binds_[kBindLStickPress]))
    buttons |= kXInputGamepadLeftThumb;
  if (IsBindPressed(key_down_, binds_[kBindRStickPress]))
    buttons |= kXInputGamepadRightThumb;
  if (IsBindPressed(key_down_, binds_[kBindBack]))
    buttons |= kXInputGamepadBack;
  if (IsBindPressed(key_down_, binds_[kBindStart]))
    buttons |= kXInputGamepadStart;
  if (IsBindPressed(key_down_, binds_[kBindGuide]))
    buttons |= kXInputGamepadGuide;
  if (IsBindPressed(key_down_, binds_[kBindDPadUp]))
    buttons |= kXInputGamepadDPadUp;
  if (IsBindPressed(key_down_, binds_[kBindDPadDown]))
    buttons |= kXInputGamepadDPadDown;
  if (IsBindPressed(key_down_, binds_[kBindDPadLeft]))
    buttons |= kXInputGamepadDPadLeft;
  if (IsBindPressed(key_down_, binds_[kBindDPadRight]))
    buttons |= kXInputGamepadDPadRight;

  u8 lt = IsBindPressed(key_down_, binds_[kBindLeftTrigger]) ? 0xFF : 0;
  u8 rt = IsBindPressed(key_down_, binds_[kBindRightTrigger]) ? 0xFF : 0;

  i32 lx = 0;
  i32 ly = 0;
  if (IsBindPressed(key_down_, binds_[kBindLStickLeft]))
    lx -= INT16_MAX;
  if (IsBindPressed(key_down_, binds_[kBindLStickRight]))
    lx += INT16_MAX;
  if (IsBindPressed(key_down_, binds_[kBindLStickUp]))
    ly += INT16_MAX;
  if (IsBindPressed(key_down_, binds_[kBindLStickDown]))
    ly -= INT16_MAX;

  i32 rx = 0;
  i32 ry = 0;
  if (IsBindPressed(key_down_, binds_[kBindRStickLeft]))
    rx -= INT16_MAX;
  if (IsBindPressed(key_down_, binds_[kBindRStickRight]))
    rx += INT16_MAX;
  if (IsBindPressed(key_down_, binds_[kBindRStickUp]))
    ry += INT16_MAX;
  if (IsBindPressed(key_down_, binds_[kBindRStickDown]))
    ry -= INT16_MAX;

  if (REXCVAR_GET(cvars::mnk_mouse)) {
    f64 sensitivity = REXCVAR_GET(cvars::mnk_sensitivity);
    constexpr f64 kBaseScale = 200.0;
    rx += static_cast<i32>(static_cast<f64>(mouse_dx_) * sensitivity * kBaseScale);
    ry += static_cast<i32>(static_cast<f64>(-mouse_dy_) * sensitivity * kBaseScale);
  }
  // Drained unconditionally, else a backlog from while the mouse was off snaps the camera.
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
    return std::unexpected(
        BuildError(HIDError::kNotConnected, "keyboard and mouse emulation is off"));
  }
  return Ok();
}

Result<X_INPUT_KEYSTROKE> KeyboardMouseDriver::GetDeviceKeystroke(DeviceId id, u32) {
  if (!IsEnabled() || id != kKeyboardMouseDevice) {
    return std::unexpected(
        BuildError(HIDError::kNotConnected, "keyboard and mouse emulation is off"));
  }
  std::lock_guard lock(state_mutex_);
  if (keystroke_queue_.empty()) {
    return std::unexpected(BuildError(HIDError::kEmpty, "no keystroke is queued"));
  }
  X_INPUT_KEYSTROKE keystroke = keystroke_queue_.front();
  keystroke_queue_.pop();
  return keystroke;
}

void KeyboardMouseDriver::OnEvent(const InputEvent& event) {
  switch (event.kind) {
    case InputEvent::Kind::kKeyDown:
      OnKey(event, true);
      break;
    case InputEvent::Kind::kKeyUp:
      OnKey(event, false);
      break;
    case InputEvent::Kind::kKeyChar:
      OnChar(event);
      break;
    case InputEvent::Kind::kButtonDown:
      OnButton(event, true);
      break;
    case InputEvent::Kind::kButtonUp:
      OnButton(event, false);
      break;
    case InputEvent::Kind::kMotion:
      OnMotion(event);
      break;
    case InputEvent::Kind::kFocusLost:
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
  ks.flags = down ? kXInputKeystrokeKeyDown : kXInputKeystrokeKeyUp;
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

  u16 flags = down ? kXInputKeystrokeKeyDown : kXInputKeystrokeKeyUp;
  if (down && event.repeat) {
    flags |= kXInputKeystrokeRepeat;
  }
  if (HasFlags(event.modifiers, KeyModifiers::kShift)) {
    flags |= kXInputKeystrokeShift;
  }
  if (HasFlags(event.modifiers, KeyModifiers::kCtrl)) {
    flags |= kXInputKeystrokeCtrl;
  }
  if (HasFlags(event.modifiers, KeyModifiers::kAlt)) {
    flags |= kXInputKeystrokeAlt;
  }
  ks.flags = flags;
  PushKeystroke(ks);
}

void KeyboardMouseDriver::RefreshBoundKeystrokesLocked() {
  u32 pressed = 0;
  for (size_t i = 0; i < kBindSpecs.size(); i++) {
    if (kBindSpecs[i].pad_key != VirtualKey::kNone && IsBindPressed(key_down_, binds_[i])) {
      pressed |= u32{1} << i;
    }
  }

  const u32 changed = pressed ^ bound_pressed_;
  bound_pressed_ = pressed;
  if (!changed) {
    return;
  }
  for (size_t i = 0; i < kBindSpecs.size(); i++) {
    const u32 bit = u32{1} << i;
    if (changed & bit) {
      EnqueueKeystroke(static_cast<u16>(kBindSpecs[i].pad_key), (pressed & bit) != 0);
    }
  }
}

void KeyboardMouseDriver::SetKeyState(u16 key, bool down) {
  if (key < 256) {
    key_down_[key] = down;
  }
}

void KeyboardMouseDriver::OnKey(const InputEvent& event, bool down) {
  if (!IsEnabled() || (down && !hub_.IsFocused())) {
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
  // WM_CHAR delivers the codepoint after its key-down, so it attaches to the queued keystroke.
  std::lock_guard lock(state_mutex_);
  if (keystroke_queue_.empty()) {
    return;
  }
  X_INPUT_KEYSTROKE& last = keystroke_queue_.back();
  const u16 flags = last.flags;
  if (!(flags & kXInputKeystrokeKeyDown) || last.unicode != 0) {
    return;
  }
  last.unicode = static_cast<u16>(event.character);
  last.flags = static_cast<u16>(flags | kXInputKeystrokeValidUnicode);
}

void KeyboardMouseDriver::OnButton(const InputEvent& event, bool down) {
  if (!IsEnabled() || (down && !hub_.IsFocused())) {
    return;
  }
  std::lock_guard lock(state_mutex_);
  switch (event.button) {
    case MouseButton::kLeft:
      SetKeyState(std::to_underlying(VirtualKey::kLButton), down);
      break;
    case MouseButton::kRight:
      SetKeyState(std::to_underlying(VirtualKey::kRButton), down);
      break;
    case MouseButton::kMiddle:
      SetKeyState(std::to_underlying(VirtualKey::kMButton), down);
      break;
    default:
      break;
  }
  RefreshBoundKeystrokesLocked();
}

void KeyboardMouseDriver::OnMotion(const InputEvent& event) {
  if (!IsEnabled() || !hub_.IsFocused()) {
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
  RefreshBoundKeystrokesLocked();
  mouse_dx_ = 0.0f;
  mouse_dy_ = 0.0f;
}

}  // namespace rex::hid
