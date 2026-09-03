/**
 * @file        input/mnk/mnk_input_driver.cpp
 * @brief       Keyboard/mouse input driver implementation.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/input/mnk/mnk_input_driver.h>

#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace rex::input::mnk {

using rex::ui::VirtualKey;

namespace {

// A single device, so its handle is a constant.
constexpr rex::input::DeviceId kMnkDevice = static_cast<rex::input::DeviceId>(0x4D4E4B00);

// Bounds the queue for titles that never call XamInputGetKeystroke.
constexpr size_t kMaxQueuedKeystrokes = 256;

// Bind values are a comma-separated list of alternatives, each optionally
// carrying modifier prefixes: "Q,I" or "Shift+W". Modifier matching is exact,
// so "Up" stays silent while Shift is held and "Shift+Up" stays silent without
// it. That is what lets the D-pad share the arrow keys. A consequence is
// that binding a bare modifier name ("Shift") can never fire, since holding it
// makes the live mask non-zero while the bind wants zero.
constexpr uint8_t kModShift = 1u << 0;
constexpr uint8_t kModCtrl = 1u << 1;
constexpr uint8_t kModAlt = 1u << 2;

uint8_t LiveModifiers(const bool (&key_down)[256]) {
  uint8_t mods = 0;
  if (key_down[static_cast<uint16_t>(rex::ui::VirtualKey::kShift)])
    mods |= kModShift;
  if (key_down[static_cast<uint16_t>(rex::ui::VirtualKey::kControl)])
    mods |= kModCtrl;
  if (key_down[static_cast<uint16_t>(rex::ui::VirtualKey::kMenu)])
    mods |= kModAlt;
  return mods;
}

// Strips leading modifier prefixes off 'token', advancing it to the bare key
// name and returning the mask they require.
uint8_t TakeModifiers(std::string_view& token) {
  uint8_t mods = 0;
  for (;;) {
    size_t plus = token.find('+');
    if (plus == std::string_view::npos || plus == 0) {
      break;
    }
    std::string_view head = token.substr(0, plus);
    uint8_t bit = 0;
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

bool TokenPressed(const bool (&key_down)[256], std::string_view token, uint8_t live_mods) {
  uint8_t want = TakeModifiers(token);
  if (want != live_mods) {
    return false;
  }
  rex::ui::VirtualKey vk = rex::ui::ParseVirtualKey(token);
  if (vk == rex::ui::VirtualKey::kNone) {
    return false;
  }
  uint16_t idx = static_cast<uint16_t>(vk);
  return idx < 256 && key_down[idx];
}

std::atomic<bool> mouse_look_active{true};

// ui::VirtualKey follows Windows VK_ numbering; the guest wants USB HID usage
// page 0x07.
uint8_t VirtualKeyToHIDUsage(rex::ui::VirtualKey vk_enum) {
  const uint32_t vk = static_cast<uint32_t>(vk_enum);

  // The runs below are contiguous in both numbering schemes.
  if (vk >= 'A' && vk <= 'Z') {
    return static_cast<uint8_t>(vk - 'A' + 0x04);
  }
  // 0 is irregular in both digit runs; handled in the switch.
  if (vk >= '1' && vk <= '9') {
    return static_cast<uint8_t>(vk - '1' + 0x1E);
  }
  if (vk >= static_cast<uint32_t>(VirtualKey::kF1) &&
      vk <= static_cast<uint32_t>(VirtualKey::kF12)) {
    return static_cast<uint8_t>(vk - static_cast<uint32_t>(VirtualKey::kF1) + 0x3A);
  }
  if (vk >= static_cast<uint32_t>(VirtualKey::kF13) &&
      vk <= static_cast<uint32_t>(VirtualKey::kF24)) {
    return static_cast<uint8_t>(vk - static_cast<uint32_t>(VirtualKey::kF13) + 0x68);
  }
  if (vk >= static_cast<uint32_t>(VirtualKey::kNumpad1) &&
      vk <= static_cast<uint32_t>(VirtualKey::kNumpad9)) {
    return static_cast<uint8_t>(vk - static_cast<uint32_t>(VirtualKey::kNumpad1) + 0x59);
  }
  if (vk >= static_cast<uint32_t>(VirtualKey::kLShift) &&
      vk <= static_cast<uint32_t>(VirtualKey::kLMenu)) {
    return static_cast<uint8_t>(vk - static_cast<uint32_t>(VirtualKey::kLShift) + 0xE1);
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
  const std::string& keys;
  VirtualKey pad_key;
};

// Rebuilt per call because the cvars are live-editable.
std::array<PadBind, 24> PadBinds() {
  return {{
      {REXCVAR_GET(keybind_a), VirtualKey::kXInputPadA},
      {REXCVAR_GET(keybind_b), VirtualKey::kXInputPadB},
      {REXCVAR_GET(keybind_x), VirtualKey::kXInputPadX},
      {REXCVAR_GET(keybind_y), VirtualKey::kXInputPadY},
      {REXCVAR_GET(keybind_left_shoulder), VirtualKey::kXInputPadLShoulder},
      {REXCVAR_GET(keybind_right_shoulder), VirtualKey::kXInputPadRShoulder},
      {REXCVAR_GET(keybind_left_trigger), VirtualKey::kXInputPadLTrigger},
      {REXCVAR_GET(keybind_right_trigger), VirtualKey::kXInputPadRTrigger},
      {REXCVAR_GET(keybind_lstick_press), VirtualKey::kXInputPadLThumbPress},
      {REXCVAR_GET(keybind_rstick_press), VirtualKey::kXInputPadRThumbPress},
      {REXCVAR_GET(keybind_back), VirtualKey::kXInputPadBack},
      {REXCVAR_GET(keybind_start), VirtualKey::kXInputPadStart},
      {REXCVAR_GET(keybind_dpad_up), VirtualKey::kXInputPadDpadUp},
      {REXCVAR_GET(keybind_dpad_down), VirtualKey::kXInputPadDpadDown},
      {REXCVAR_GET(keybind_dpad_left), VirtualKey::kXInputPadDpadLeft},
      {REXCVAR_GET(keybind_dpad_right), VirtualKey::kXInputPadDpadRight},
      {REXCVAR_GET(keybind_lstick_up), VirtualKey::kXInputPadLThumbUp},
      {REXCVAR_GET(keybind_lstick_down), VirtualKey::kXInputPadLThumbDown},
      {REXCVAR_GET(keybind_lstick_left), VirtualKey::kXInputPadLThumbLeft},
      {REXCVAR_GET(keybind_lstick_right), VirtualKey::kXInputPadLThumbRight},
      {REXCVAR_GET(keybind_rstick_up), VirtualKey::kXInputPadRThumbUp},
      {REXCVAR_GET(keybind_rstick_down), VirtualKey::kXInputPadRThumbDown},
      {REXCVAR_GET(keybind_rstick_left), VirtualKey::kXInputPadRThumbLeft},
      {REXCVAR_GET(keybind_rstick_right), VirtualKey::kXInputPadRThumbRight},
  }};
}

}  // namespace

void SetMouseLookActive(bool active) {
  mouse_look_active.store(active, std::memory_order_relaxed);
}

bool IsMouseLookActive() {
  return mouse_look_active.load(std::memory_order_relaxed);
}

MnkInputDriver::MnkInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

MnkInputDriver::~MnkInputDriver() {
  // Detach handled by OnClosing; if window outlives the driver, clean up here.
  DetachFromWindow();
}

X_STATUS MnkInputDriver::Setup() {
  return X_STATUS_SUCCESS;
}

void MnkInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window) {
    {
      std::lock_guard lock(state_mutex_);
      attached_window_ = window;
    }
    window->AddInputListener(this, window_z_order());
    window->AddListener(this);
  }
}

void MnkInputDriver::OnClosing(rex::ui::UIEvent&) {
  DetachFromWindow();
}

void MnkInputDriver::DetachFromWindow() {
  rex::ui::Window* window = attached_window_;
  if (!window) {
    return;
  }
  window->app_context().CallInUIThreadSynchronous([this, window] {
    // Detach first so nothing new is queued, then run out what already was.
    {
      std::lock_guard lock(state_mutex_);
      attached_window_ = nullptr;
    }
    if (mouse_capture_update_queued_.load(std::memory_order_relaxed)) {
      window->app_context().ExecutePendingFunctionsFromUIThread();
    }
    ReleaseMouseCaptureFromUIThread(window);
    window->RemoveInputListener(this);
    window->RemoveListener(this);
  });
}

bool MnkInputDriver::IsEnabled() const {
  // Passthrough enables the device on its own.
  return REXCVAR_GET(mnk_mode) || REXCVAR_GET(mnk_passthrough);
}

static bool IsPassthroughEnabled() {
  return REXCVAR_GET(mnk_passthrough);
}

static bool IsBindPressed(const bool (&key_down)[256], const std::string& cvar_val) {
  const uint8_t live_mods = LiveModifiers(key_down);
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

void MnkInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  // Disabled means no device at all, so it never occupies a guest user slot.
  if (!IsEnabled()) {
    return;
  }
  DeviceInfo info;
  info.id = kMnkDevice;
  info.name = IsPassthroughEnabled() ? "Keyboard" : "Keyboard and Mouse";
  info.subtype =
      IsPassthroughEnabled() ? XINPUT_DEVSUBTYPE_USB_KEYBOARD : XINPUT_DEVSUBTYPE_GAMEPAD;
  info.synthetic = true;
  out.push_back(info);
}

X_RESULT MnkInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                               X_INPUT_CAPABILITIES* out_caps) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    if (IsPassthroughEnabled()) {
      out_caps->type = XINPUT_DEVTYPE_KEYBOARD;
      out_caps->sub_type = XINPUT_DEVSUBTYPE_USB_KEYBOARD;
      return X_ERROR_SUCCESS;
    }
    out_caps->type = XINPUT_DEVTYPE_GAMEPAD;
    out_caps->sub_type = XINPUT_DEVSUBTYPE_GAMEPAD;
    out_caps->flags = 0;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Keys reach the guest through GetDeviceKeystroke only, leaving the guest
  // user free for a real controller.
  if (IsPassthroughEnabled()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Mouse look is opt in. Without this gate, keyboard input alone would hide
  // and lock the cursor, breaking the ImGui overlays.
  QueueMouseCaptureUpdate(IsEnabled() && REXCVAR_GET(mnk_mouse) && IsMouseLookActive() &&
                          has_focus_ && is_active());

  if (!is_active() || !has_focus_) {
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet_number_;
    }
    return X_ERROR_SUCCESS;
  }

  std::lock_guard lock(state_mutex_);

  uint16_t buttons = 0;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_a)))
    buttons |= X_INPUT_GAMEPAD_A;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_b)))
    buttons |= X_INPUT_GAMEPAD_B;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_x)))
    buttons |= X_INPUT_GAMEPAD_X;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_y)))
    buttons |= X_INPUT_GAMEPAD_Y;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_left_shoulder)))
    buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_right_shoulder)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_press)))
    buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_rstick_press)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_back)))
    buttons |= X_INPUT_GAMEPAD_BACK;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_start)))
    buttons |= X_INPUT_GAMEPAD_START;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_guide)))
    buttons |= X_INPUT_GAMEPAD_GUIDE;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_up)))
    buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_down)))
    buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_left)))
    buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_right)))
    buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;

  uint8_t lt = IsBindPressed(key_down_, REXCVAR_GET(keybind_left_trigger)) ? 0xFF : 0;
  uint8_t rt = IsBindPressed(key_down_, REXCVAR_GET(keybind_right_trigger)) ? 0xFF : 0;

  int32_t lx = 0;
  int32_t ly = 0;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_left)))
    lx -= INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_right)))
    lx += INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_up)))
    ly += INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_down)))
    ly -= INT16_MAX;

  int32_t rx = 0;
  int32_t ry = 0;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_rstick_left)))
    rx -= INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_rstick_right)))
    rx += INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_rstick_up)))
    ry += INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_rstick_down)))
    ry -= INT16_MAX;

  if (REXCVAR_GET(mnk_mouse) && IsMouseLookActive()) {
    double sensitivity = REXCVAR_GET(mnk_sensitivity);
    constexpr double kBaseScale = 200.0;
    rx += static_cast<int32_t>(double(mouse_dx_) * sensitivity * kBaseScale);
    ry += static_cast<int32_t>(double(-mouse_dy_) * sensitivity * kBaseScale);
  }
  // Drained unconditionally: deltas keep accumulating in OnMouseMove while the
  // mouse is off, and toggling it on would otherwise dump the whole backlog
  // into one frame as a camera snap.
  mouse_dx_ = 0.0f;
  mouse_dy_ = 0.0f;

  auto clamp16 = [](int32_t v) -> int16_t {
    return static_cast<int16_t>(std::clamp(v, (int32_t)INT16_MIN, (int32_t)INT16_MAX));
  };

  packet_number_++;

  if (out_state) {
    out_state->packet_number = packet_number_;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = lt;
    out_state->gamepad.right_trigger = rt;
    out_state->gamepad.thumb_lx = clamp16(lx);
    out_state->gamepad.thumb_ly = clamp16(ly);
    out_state->gamepad.thumb_rx = clamp16(rx);
    out_state->gamepad.thumb_ry = clamp16(ry);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                            X_INPUT_KEYSTROKE* out_keystroke) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard lock(state_mutex_);
  if (keystroke_queue_.empty()) {
    return X_ERROR_EMPTY;
  }
  if (out_keystroke) {
    *out_keystroke = keystroke_queue_.front();
  }
  keystroke_queue_.pop();
  return X_ERROR_SUCCESS;
}

void MnkInputDriver::EnqueueKeystroke(uint16_t vk_pad, bool down) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = vk_pad;
  ks.unicode = 0;
  ks.flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  // InputSystem stamps the guest user this device is assigned to.
  ks.user_index = 0;
  ks.hid_code = 0;
  PushKeystroke(ks);
}

void MnkInputDriver::PushKeystroke(const X_INPUT_KEYSTROKE& ks) {
  if (keystroke_queue_.size() >= kMaxQueuedKeystrokes) {
    keystroke_queue_.pop();
  }
  keystroke_queue_.push(ks);
}

void MnkInputDriver::EnqueueRawKeystroke(const rex::ui::KeyEvent& e, bool down) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = static_cast<uint16_t>(e.virtual_key());
  ks.unicode = 0;
  ks.user_index = 0;
  ks.hid_code = VirtualKeyToHIDUsage(e.virtual_key());

  uint16_t flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  // Already down, so this is the OS auto-repeat.
  if (down && e.prev_state()) {
    flags |= X_INPUT_KEYSTROKE_REPEAT;
  }
  if (e.is_shift_pressed()) {
    flags |= X_INPUT_KEYSTROKE_SHIFT;
  }
  if (e.is_ctrl_pressed()) {
    flags |= X_INPUT_KEYSTROKE_CTRL;
  }
  if (e.is_alt_pressed()) {
    flags |= X_INPUT_KEYSTROKE_ALT;
  }
  ks.flags = flags;
  PushKeystroke(ks);
}

void MnkInputDriver::RefreshBoundKeystrokesLocked() {
  const auto binds = PadBinds();
  uint32_t pressed = 0;
  for (size_t i = 0; i < binds.size(); i++) {
    if (IsBindPressed(key_down_, binds[i].keys)) {
      pressed |= uint32_t(1) << i;
    }
  }

  // The whole table is diffed rather than the key that moved: binds carry
  // modifiers, so Shift alone can flip several of them at once.
  const uint32_t changed = pressed ^ bound_pressed_;
  bound_pressed_ = pressed;
  if (!changed) {
    return;
  }
  for (size_t i = 0; i < binds.size(); i++) {
    const uint32_t bit = uint32_t(1) << i;
    if (changed & bit) {
      EnqueueKeystroke(static_cast<uint16_t>(binds[i].pad_key), (pressed & bit) != 0);
    }
  }
}

void MnkInputDriver::QueueMouseCaptureUpdate(bool should_capture) {
  mouse_capture_requested_.store(should_capture, std::memory_order_relaxed);
  bool already_queued = false;
  if (!mouse_capture_update_queued_.compare_exchange_strong(already_queued, true)) {
    return;
  }
  std::lock_guard lock(state_mutex_);
  if (!attached_window_) {
    mouse_capture_update_queued_.store(false, std::memory_order_relaxed);
    return;
  }
  // Deferred, not CallInUIThread: running inline would re-enter state_mutex_.
  attached_window_->app_context().CallInUIThreadDeferred([this] {
    mouse_capture_update_queued_.store(false, std::memory_order_relaxed);
    ApplyMouseCaptureFromUIThread();
  });
}

void MnkInputDriver::ApplyMouseCaptureFromUIThread() {
  rex::ui::Window* window = attached_window_;
  if (!window) {
    return;
  }
  bool should_capture = mouse_capture_requested_.load(std::memory_order_relaxed);
  if (should_capture == mouse_captured_) {
    return;
  }
  if (!should_capture) {
    ReleaseMouseCaptureFromUIThread(window);
    return;
  }
  mouse_captured_ = true;
  precapture_cursor_visibility_ = window->GetCursorVisibility();
  window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
  window->CaptureMouse();
  relative_mouse_mode_ = window->SetRelativeMouseMode(true);
  if (!relative_mouse_mode_) {
    REXLOG_WARN("Pointer lock unavailable, mouse look falls back to recentering the cursor");
  }
  // Reset deltas to avoid a spike on capture start
  std::lock_guard lock(state_mutex_);
  mouse_dx_ = 0.0f;
  mouse_dy_ = 0.0f;
}

void MnkInputDriver::ReleaseMouseCaptureFromUIThread(rex::ui::Window* window) {
  if (!mouse_captured_) {
    return;
  }
  mouse_captured_ = false;
  window->SetCursorVisibility(precapture_cursor_visibility_);
  window->SetRelativeMouseMode(false);
  relative_mouse_mode_ = false;
  window->ReleaseMouse();
}

void MnkInputDriver::RecenterCursorFromUIThread(int32_t x, int32_t y) {
  rex::ui::Window* window = attached_window_;
  if (!window) {
    return;
  }
  int32_t width = int32_t(window->GetActualPhysicalWidth());
  int32_t height = int32_t(window->GetActualPhysicalHeight());
  if (width <= 0 || height <= 0) {
    return;
  }
  // Only once the pointer has drifted well off the middle, to keep warps rare.
  if (std::abs(x - width / 2) < width / 4 && std::abs(y - height / 2) < height / 4) {
    return;
  }
  int32_t center_x = 0;
  int32_t center_y = 0;
  if (window->WarpMouseToCenter(center_x, center_y)) {
    prev_mouse_x_ = center_x;
    prev_mouse_y_ = center_y;
  }
}

void MnkInputDriver::SetKeyState(uint16_t vk, bool down) {
  if (vk < 256) {
    key_down_[vk] = down;
  }
}

void MnkInputDriver::OnKeyDown(rex::ui::KeyEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  if (IsPassthroughEnabled()) {
    EnqueueRawKeystroke(e, true);
    return;
  }
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  SetKeyState(vk, true);
  RefreshBoundKeystrokesLocked();
}

void MnkInputDriver::OnKeyUp(rex::ui::KeyEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  if (IsPassthroughEnabled()) {
    EnqueueRawKeystroke(e, false);
    return;
  }
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  SetKeyState(vk, false);
  RefreshBoundKeystrokesLocked();
}

void MnkInputDriver::OnKeyChar(rex::ui::KeyEvent& e) {
  if (!IsEnabled() || !IsPassthroughEnabled())
    return;
  // WM_CHAR convention: the codepoint arrives after its key-down, so it is
  // attached to the keystroke already queued for that key.
  std::lock_guard lock(state_mutex_);
  if (keystroke_queue_.empty()) {
    return;
  }
  X_INPUT_KEYSTROKE& last = keystroke_queue_.back();
  const uint16_t flags = last.flags;
  if (!(flags & X_INPUT_KEYSTROKE_KEYDOWN) || last.unicode != 0) {
    return;
  }
  last.unicode = static_cast<uint16_t>(e.virtual_key());
  last.flags = static_cast<uint16_t>(flags | X_INPUT_KEYSTROKE_VALIDUNICODE);
}

void MnkInputDriver::OnMouseDown(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), true);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), true);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), true);
      break;
    default:
      break;
  }
  RefreshBoundKeystrokesLocked();
}

void MnkInputDriver::OnMouseUp(rex::ui::MouseEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), false);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), false);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), false);
      break;
    default:
      break;
  }
  RefreshBoundKeystrokesLocked();
}

void MnkInputDriver::OnMouseMove(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  int32_t x = e.x();
  int32_t y = e.y();
  {
    std::lock_guard lock(state_mutex_);
    if (relative_mouse_mode_) {
      // The pointer is locked, so the absolute position no longer moves.
      mouse_dx_ += e.dx();
      mouse_dy_ += e.dy();
    } else {
      mouse_dx_ += float(x - prev_mouse_x_);
      mouse_dy_ += float(y - prev_mouse_y_);
    }
  }
  prev_mouse_x_ = x;
  prev_mouse_y_ = y;
  // Without a pointer lock the cursor still has to be kept off the edges.
  if (mouse_captured_ && !relative_mouse_mode_) {
    RecenterCursorFromUIThread(x, y);
  }
}

void MnkInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  has_focus_ = false;
  // Withdraw the request too, or an update queued before the focus loss grabs
  // the cursor straight back.
  mouse_capture_requested_.store(false, std::memory_order_relaxed);
  {
    std::lock_guard lock(state_mutex_);
    std::memset(key_down_, 0, sizeof(key_down_));
    // Releases landing on another window never arrive here as key-ups.
    RefreshBoundKeystrokesLocked();
    mouse_dx_ = 0.0f;
    mouse_dy_ = 0.0f;
  }
  if (attached_window_) {
    ReleaseMouseCaptureFromUIThread(attached_window_);
  }
}

void MnkInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {
  has_focus_ = true;
}

}  // namespace rex::input::mnk
