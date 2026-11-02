/**
 * @file        hid/keyboard_mouse_pad.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <array>
#include <cstring>
#include <ranges>
#include <utility>

#include <rex/logging/log.h>
#include <rex/hid/pad.h>

#include "hid/input_internal.h"

namespace rex::hid::detail {

namespace {

using hid::InputEvent;
using hid::KeyChord;
using hid::KeyModifiers;
using hid::MouseButton;
using hid::VirtualKey;

constexpr rex::hid::PadDeviceID kKeyboardMouseDevice =
    static_cast<rex::hid::PadDeviceID>(0x4D4E4B00);
constexpr size_t kMaxQueuedKeystrokes = 256;
constexpr f64 kMouseBaseScale = 200.0;

KeyModifiers HeldModifiers(const bool (&key_down)[256]) {
  KeyModifiers mods = KeyModifiers::kNone;
  if (key_down[std::to_underlying(VirtualKey::kShift)]) {
    mods |= KeyModifiers::kShift;
  }
  if (key_down[std::to_underlying(VirtualKey::kControl)]) {
    mods |= KeyModifiers::kCtrl;
  }
  if (key_down[std::to_underlying(VirtualKey::kMenu)]) {
    mods |= KeyModifiers::kAlt;
  }
  return mods;
}

bool IsBindPressed(const bool (&key_down)[256], const std::vector<KeyChord>& chords) {
  const KeyModifiers live = HeldModifiers(key_down);
  return std::ranges::any_of(chords, [&](const KeyChord& chord) {
    const u16 index = static_cast<u16>(chord.key);
    return chord.modifiers == live && index < 256 && key_down[index];
  });
}

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

Error NotConnected() {
  return Error(ErrorCategory::kNotFound, "keyboard and mouse emulation is off");
}

}  // namespace

KeyboardMousePad::KeyboardMousePad(rex::hid::InputSettings& settings, hid::IBindingSet& bindings,
                                   const std::atomic<bool>& focused)
    : settings_(settings), bindings_(bindings), focused_(focused) {
  RefreshBinds();
}

KeyboardMousePad::~KeyboardMousePad() = default;

void KeyboardMousePad::RefreshBinds() {
  for (const std::string& message : bindings_.TakeParseErrors()) {
    REXLOG_WARN(hid, "{}, the binding keeps its previous keys", message);
  }
  const u32 generation = bindings_.GetGeneration();
  if (generation == bind_generation_) {
    return;
  }
  bind_generation_ = generation;
  std::array<std::vector<KeyChord>, kBindCount> fresh;
  for (size_t i = 0; i < kBindCount; ++i) {
    for (const hid::InputSource& source : bindings_.GetSources(i)) {
      if (source.type == hid::InputSourceType::kKey) {
        fresh[i].push_back({static_cast<VirtualKey>(source.code), source.modifiers});
      }
    }
  }
  std::lock_guard lock(mutex_);
  binds_ = std::move(fresh);
}

bool KeyboardMousePad::IsEnabled() const {
  return settings_.mnk_mode.Get() || settings_.mnk_passthrough.Get();
}

bool KeyboardMousePad::IsPassthrough() const {
  return settings_.mnk_passthrough.Get();
}

u64 KeyboardMousePad::GetDeviceRevision() const {
  const u32 mode = (IsEnabled() ? 1u : 0u) | (IsPassthrough() ? 2u : 0u);
  if (listed_mode_.exchange(mode, std::memory_order_relaxed) != mode) {
    revision_.fetch_add(1, std::memory_order_relaxed);
  }
  return revision_.load(std::memory_order_relaxed);
}

void KeyboardMousePad::EnumerateDevices(std::vector<rex::hid::PadDevice>& out) {
  if (!IsEnabled()) {
    return;
  }
  rex::hid::PadDevice device;
  device.id = kKeyboardMouseDevice;
  device.name = IsPassthrough() ? "Keyboard" : "Keyboard and Mouse";
  out.push_back(std::move(device));
}

Result<rex::hid::PadCapabilities> KeyboardMousePad::GetDeviceCapabilities(
    rex::hid::PadDeviceID id) {
  if (!IsEnabled() || id != kKeyboardMouseDevice) {
    return std::unexpected(NotConnected());
  }
  if (IsPassthrough()) {
    rex::hid::PadCapabilities caps;
    caps.subtype = kSubtypeUSBKeyboard;
    return caps;
  }
  return MakeFullPadCapabilities();
}

bool KeyboardMousePad::IsPressed(size_t index) const {
  return IsBindPressed(key_down_, binds_[index]);
}

Result<rex::hid::PadState> KeyboardMousePad::GetDeviceState(rex::hid::PadDeviceID id) {
  if (!IsEnabled() || id != kKeyboardMouseDevice) {
    return std::unexpected(NotConnected());
  }
  if (IsPassthrough()) {
    return Err<rex::hid::PadState>(ErrorCategory::kNotFound,
                                   "passthrough keeps the user free for a pad");
  }

  const bool focused = focused_.load(std::memory_order_relaxed);
  pointer_lock_.store(settings_.mnk_mouse.Get() && focused, std::memory_order_relaxed);
  if (!focused) {
    return rex::hid::PadState{};
  }

  std::lock_guard guard(mutex_);

  rex::hid::PadState state;
  u16 buttons = 0;
  const std::array<std::pair<size_t, u16>, 15> button_binds = {{
      {kBindA, rex::hid::kPadA},
      {kBindB, rex::hid::kPadB},
      {kBindX, rex::hid::kPadX},
      {kBindY, rex::hid::kPadY},
      {kBindLeftShoulder, rex::hid::kPadLeftShoulder},
      {kBindRightShoulder, rex::hid::kPadRightShoulder},
      {kBindLStickPress, rex::hid::kPadLeftThumb},
      {kBindRStickPress, rex::hid::kPadRightThumb},
      {kBindBack, rex::hid::kPadBack},
      {kBindStart, rex::hid::kPadStart},
      {kBindGuide, rex::hid::kPadGuide},
      {kBindDPadUp, rex::hid::kPadDPadUp},
      {kBindDPadDown, rex::hid::kPadDPadDown},
      {kBindDPadLeft, rex::hid::kPadDPadLeft},
      {kBindDPadRight, rex::hid::kPadDPadRight},
  }};
  for (const auto& [index, bit] : button_binds) {
    if (IsPressed(index)) {
      buttons |= bit;
    }
  }

  i32 lx = 0;
  i32 ly = 0;
  i32 rx = 0;
  i32 ry = 0;
  if (IsPressed(kBindLStickLeft)) {
    lx -= INT16_MAX;
  }
  if (IsPressed(kBindLStickRight)) {
    lx += INT16_MAX;
  }
  if (IsPressed(kBindLStickUp)) {
    ly += INT16_MAX;
  }
  if (IsPressed(kBindLStickDown)) {
    ly -= INT16_MAX;
  }
  if (IsPressed(kBindRStickLeft)) {
    rx -= INT16_MAX;
  }
  if (IsPressed(kBindRStickRight)) {
    rx += INT16_MAX;
  }
  if (IsPressed(kBindRStickUp)) {
    ry += INT16_MAX;
  }
  if (IsPressed(kBindRStickDown)) {
    ry -= INT16_MAX;
  }

  if (settings_.mnk_mouse.Get()) {
    const f64 sensitivity = settings_.mnk_sensitivity.Get();
    rx += static_cast<i32>(static_cast<f64>(mouse_dx_) * sensitivity * kMouseBaseScale);
    ry += static_cast<i32>(static_cast<f64>(-mouse_dy_) * sensitivity * kMouseBaseScale);
  }
  mouse_dx_ = 0.0f;
  mouse_dy_ = 0.0f;

  auto clamp16 = [](i32 value) {
    return static_cast<i16>(
        std::clamp(value, static_cast<i32>(INT16_MIN), static_cast<i32>(INT16_MAX)));
  };
  state.buttons = buttons;
  state.left_trigger = IsPressed(kBindLeftTrigger) ? 0xFF : 0;
  state.right_trigger = IsPressed(kBindRightTrigger) ? 0xFF : 0;
  state.thumb_lx = clamp16(lx);
  state.thumb_ly = clamp16(ly);
  state.thumb_rx = clamp16(rx);
  state.thumb_ry = clamp16(ry);
  return state;
}

Result<void> KeyboardMousePad::SetDeviceVibration(rex::hid::PadDeviceID id,
                                                  const rex::hid::PadVibration&) {
  if (!IsEnabled() || id != kKeyboardMouseDevice) {
    return std::unexpected(NotConnected());
  }
  return {};
}

Result<rex::hid::PadKeystroke> KeyboardMousePad::TakeKeystroke() {
  if (!IsEnabled() || !IsPassthrough()) {
    return std::unexpected(NotConnected());
  }
  std::lock_guard lock(mutex_);
  if (queue_.empty()) {
    return Err<rex::hid::PadKeystroke>(ErrorCategory::kNotFound, "no keystroke is queued");
  }
  rex::hid::PadKeystroke keystroke = queue_.front();
  queue_.pop_front();
  return keystroke;
}

void KeyboardMousePad::OnEvent(InputEvent& event) {
  RefreshBinds();
  switch (event.type) {
    case InputEvent::Type::kKeyDown:
      OnKey(event, true);
      break;
    case InputEvent::Type::kKeyUp:
      OnKey(event, false);
      break;
    case InputEvent::Type::kKeyChar:
      OnChar(event);
      break;
    case InputEvent::Type::kButtonDown:
      OnButton(event, true);
      break;
    case InputEvent::Type::kButtonUp:
      OnButton(event, false);
      break;
    case InputEvent::Type::kMotion:
      OnMotion(event);
      break;
    case InputEvent::Type::kFocusLost:
      OnFocusLost();
      break;
    default:
      break;
  }
}

void KeyboardMousePad::PushKeystroke(const rex::hid::PadKeystroke& keystroke) {
  if (queue_.size() >= kMaxQueuedKeystrokes) {
    queue_.pop_front();
  }
  queue_.push_back(keystroke);
}

void KeyboardMousePad::SetKeyState(u16 key, bool down) {
  if (key < 256) {
    key_down_[key] = down;
  }
}

void KeyboardMousePad::OnKey(const InputEvent& event, bool down) {
  if (!IsEnabled() || (down && !focused_.load(std::memory_order_relaxed))) {
    return;
  }
  std::lock_guard lock(mutex_);
  if (IsPassthrough()) {
    rex::hid::PadKeystroke keystroke;
    keystroke.virtual_key = static_cast<u16>(event.key);
    keystroke.hid_code = VirtualKeyToHIDUsage(event.key);
    u16 flags = down ? kKeystrokeKeyDown : kKeystrokeKeyUp;
    if (down && event.repeat) {
      flags |= kKeystrokeRepeat;
    }
    if (HasFlags(event.modifiers, KeyModifiers::kShift)) {
      flags |= kKeystrokeShift;
    }
    if (HasFlags(event.modifiers, KeyModifiers::kCtrl)) {
      flags |= kKeystrokeCtrl;
    }
    if (HasFlags(event.modifiers, KeyModifiers::kAlt)) {
      flags |= kKeystrokeAlt;
    }
    keystroke.flags = flags;
    PushKeystroke(keystroke);
    return;
  }
  SetKeyState(static_cast<u16>(event.key), down);
}

void KeyboardMousePad::OnChar(const InputEvent& event) {
  if (!IsEnabled() || !IsPassthrough()) {
    return;
  }
  std::lock_guard lock(mutex_);
  if (queue_.empty()) {
    return;
  }
  rex::hid::PadKeystroke& last = queue_.back();
  if (!(last.flags & kKeystrokeKeyDown) || last.unicode != 0) {
    return;
  }
  last.unicode = static_cast<u16>(event.character);
  last.flags = static_cast<u16>(last.flags | kKeystrokeValidUnicode);
}

void KeyboardMousePad::OnButton(const InputEvent& event, bool down) {
  if (!IsEnabled() || (down && !focused_.load(std::memory_order_relaxed))) {
    return;
  }
  std::lock_guard lock(mutex_);
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
    case MouseButton::kX1:
      SetKeyState(std::to_underlying(VirtualKey::kXButton1), down);
      break;
    case MouseButton::kX2:
      SetKeyState(std::to_underlying(VirtualKey::kXButton2), down);
      break;
    case MouseButton::kNone:
      break;
  }
}

void KeyboardMousePad::OnMotion(const InputEvent& event) {
  if (!IsEnabled() || !focused_.load(std::memory_order_relaxed)) {
    return;
  }
  std::lock_guard lock(mutex_);
  mouse_dx_ += event.dx;
  mouse_dy_ += event.dy;
}

void KeyboardMousePad::OnFocusLost() {
  pointer_lock_.store(false, std::memory_order_relaxed);
  std::lock_guard lock(mutex_);
  std::memset(key_down_, 0, sizeof(key_down_));
  mouse_dx_ = 0.0f;
  mouse_dy_ = 0.0f;
}

}  // namespace rex::hid::detail
