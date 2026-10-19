#include "hid/hub.h"

#include <algorithm>
#include <cmath>

#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging/log.h>
#include <rex/move_only_function.h>

#include "hid/cvar_table.h"
#include "hid/idle_pad_driver.h"
#include "hid/keyboard_mouse_driver.h"

namespace rex::hid {

namespace {

constexpr u32 kSyntheticOrdinal = UINT32_MAX;

rex::Error NotConnected() {
  return BuildError(HIDError::kNotConnected, "no device answers for the user");
}

u32 ButtonBit(MouseButton button) {
  return 1u << static_cast<u32>(button);
}

}  // namespace

Hub::Hub() : assignment_(std::make_unique<SlotAssignment>()) {
  auto keyboard_mouse = std::make_unique<KeyboardMouseDriver>(*this);
  keyboard_mouse_ = keyboard_mouse.get();
  drivers_.push_back(std::move(keyboard_mouse));
  drivers_.push_back(std::make_unique<IdlePadDriver>());
}

Hub::~Hub() = default;

void Hub::AttachCvars(cvar::ICvarRegistry& cvars) {
  keyboard_mouse_->Attach(cvars);
}

void Hub::DetachCvars() {
  keyboard_mouse_->Detach();
}

void Hub::AddPadDriver(std::unique_ptr<Driver> driver) {
  std::lock_guard lock(mutex_);
  driver->AttachHost(host_.load(std::memory_order_acquire));
  drivers_.insert(drivers_.begin() + static_cast<std::ptrdiff_t>(pad_drivers_), std::move(driver));
  ++pad_drivers_;
}

void Hub::ClearPadDrivers() {
  std::lock_guard lock(mutex_);
  devices_.clear();
  device_owners_.clear();
  drivers_.erase(drivers_.begin(), drivers_.begin() + static_cast<std::ptrdiff_t>(pad_drivers_));
  pad_drivers_ = 0;
  if (assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
}

void Hub::SetDeviceAssignment(std::unique_ptr<IDeviceAssignment> assignment) {
  std::lock_guard lock(mutex_);
  assignment_ = std::move(assignment);
  if (assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
}

void Hub::RefreshDevicesLocked() {
  std::vector<DeviceInfo> seen;
  std::vector<Driver*> owners;
  std::vector<DeviceInfo> enumerated;
  for (auto& driver : drivers_) {
    enumerated.clear();
    driver->EnumerateDevices(enumerated);
    for (auto& info : enumerated) {
      seen.push_back(info);
      owners.push_back(driver.get());
    }
  }

  bool changed = seen.size() != devices_.size();
  std::vector<bool> fresh(seen.size(), false);
  for (size_t i = 0; i < seen.size(); i++) {
    auto existing = std::find_if(devices_.begin(), devices_.end(),
                                 [&](const DeviceInfo& d) { return d.id == seen[i].id; });
    if (existing != devices_.end()) {
      seen[i].ordinal = existing->ordinal;
      continue;
    }
    fresh[i] = true;
    changed = true;
  }

  // Lowest free ordinal, so reconnecting pads never run past the title users.
  for (size_t i = 0; i < seen.size(); i++) {
    if (!fresh[i]) {
      continue;
    }
    // Synthetic devices take no ordinal, so the first pad is title user 0.
    if (seen[i].synthetic) {
      seen[i].ordinal = kSyntheticOrdinal;
      fresh[i] = false;
      continue;
    }
    auto taken = [&](u32 candidate) {
      for (size_t j = 0; j < seen.size(); j++) {
        if (!fresh[j] && !seen[j].synthetic && seen[j].ordinal == candidate) {
          return true;
        }
      }
      return false;
    };
    u32 ordinal = 0;
    while (taken(ordinal)) {
      ordinal++;
    }
    seen[i].ordinal = ordinal;
    fresh[i] = false;
  }

  for (const auto& old : devices_) {
    if (std::none_of(seen.begin(), seen.end(),
                     [&](const DeviceInfo& d) { return d.id == old.id; })) {
      active_devices_.Forget(old.id);
      changed = true;
    }
  }

  std::vector<size_t> order(seen.size());
  for (size_t i = 0; i < order.size(); i++) {
    order[i] = i;
  }
  // Stable, since synthetic devices share 1 ordinal and their order picks the capabilities source.
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return seen[a].ordinal < seen[b].ordinal; });

  devices_.clear();
  device_owners_.clear();
  for (size_t i : order) {
    devices_.push_back(seen[i]);
    device_owners_.push_back(owners[i]);
  }

  if (changed && assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
}

Driver* Hub::DriverForDeviceLocked(DeviceId id) {
  for (size_t i = 0; i < devices_.size(); i++) {
    if (devices_[i].id == id) {
      return device_owners_[i];
    }
  }
  return nullptr;
}

const DeviceInfo* Hub::DeviceInfoForLocked(DeviceId id) const {
  for (const auto& device : devices_) {
    if (device.id == id) {
      return &device;
    }
  }
  return nullptr;
}

DeviceId Hub::ChooseDeviceForUserLocked(u32 user) const {
  if (!assignment_) {
    return DeviceId::kInvalid;
  }
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user, ids);
  if (ids.empty()) {
    return DeviceId::kInvalid;
  }
  DeviceId chosen = active_devices_.Active(user);
  if (std::find(ids.begin(), ids.end(), chosen) == ids.end()) {
    chosen = ids.front();
  }
  return chosen;
}

Result<X_INPUT_CAPABILITIES> Hub::GetCapabilities(u32 user, u32 flags) {
  REX_PROFILE_SCOPE("hid");
  std::lock_guard lock(mutex_);
  if (!assignment_) {
    return std::unexpected(NotConnected());
  }
  RefreshDevicesLocked();
  const DeviceId chosen = ChooseDeviceForUserLocked(user);
  Driver* driver = DriverForDeviceLocked(chosen);
  if (!driver) {
    return std::unexpected(NotConnected());
  }
  return driver->GetDeviceCapabilities(chosen, flags);
}

void Hub::UpdateConnectedUserLocked(u32 user, bool connected) {
  if (user >= kMaxTitleUsers || connected_users_.test(user) == connected) {
    return;
  }
  connected_users_.set(user, connected);
  if (connected) {
    REXLOG_INFO(hid, "New controller connected to user {}.", user);
  } else {
    REXLOG_INFO(hid, "Controller disconnected from user {}.", user);
  }

  // Titles read capabilities only on this notification, so a hot plugged pad needs it.
  if (connection_listener_) {
    connection_listener_(user, connected);
  }

  if (!connected) {
    user_max_joystick_value_[user] = {};
    consumed_buttons_[user] = 0;
    return;
  }

  // Deadzone percentages scale against the device's own range.
  const DeviceId chosen = ChooseDeviceForUserLocked(user);
  Driver* driver = DriverForDeviceLocked(chosen);
  if (!driver) {
    return;
  }
  auto caps = driver->GetDeviceCapabilities(chosen, 0);
  if (!caps) {
    return;
  }
  user_max_joystick_value_[user] = {{caps->gamepad.thumb_lx, caps->gamepad.thumb_ly},
                                    {caps->gamepad.thumb_rx, caps->gamepad.thumb_ry}};
}

Result<X_INPUT_STATE> Hub::GetState(u32 user) {
  REX_PROFILE_SCOPE("hid");
  TrackCapture();
  std::lock_guard lock(mutex_);
  if (ui_blocks_.load() > 0) {
    return X_INPUT_STATE{};
  }
  auto state = ReadStateLocked(user);
  if (state && user < kMaxTitleUsers && consumed_buttons_[user] != 0) {
    const u16 buttons = state->gamepad.buttons;
    consumed_buttons_[user] &= buttons;
    state->gamepad.buttons = static_cast<u16>(buttons & ~consumed_buttons_[user]);
  }
  return state;
}

Result<X_INPUT_STATE> Hub::ReadStateLocked(u32 user) {
  REX_PROFILE_SCOPE("hid");
  if (!assignment_) {
    return std::unexpected(NotConnected());
  }

  RefreshDevicesLocked();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user, ids);

  X_INPUT_STATE merged = {};
  bool any = false;
  for (DeviceId id : ids) {
    Driver* driver = DriverForDeviceLocked(id);
    if (!driver) {
      continue;
    }
    auto state = driver->GetDeviceState(id);
    if (!state) {
      continue;
    }
    active_devices_.Observe(user, id, state->gamepad);
    if (!any) {
      merged = *state;
      any = true;
    } else {
      MergeInto(merged, *state);
    }
  }

  if (!any) {
    UpdateConnectedUserLocked(user, false);
    return std::unexpected(NotConnected());
  }
  UpdateConnectedUserLocked(user, true);
  AdjustDeadzoneLevels(user, merged.gamepad);
  return merged;
}

void Hub::AcquireUIBlock() {
  ui_blocks_.fetch_add(1);
}

void Hub::ReleaseUIBlock() {
  std::lock_guard lock(mutex_);
  // Held buttons stay masked until released so the dismissing press never reaches the title.
  for (u32 user = 0; user < kMaxTitleUsers; user++) {
    if (auto state = ReadStateLocked(user)) {
      consumed_buttons_[user] |= static_cast<u16>(state->gamepad.buttons);
    }
  }
  ui_blocks_.fetch_sub(1);
}

void Hub::AdjustDeadzoneLevels(u32 user, X_INPUT_GAMEPAD& gamepad) const {
  if (user >= kMaxTitleUsers) {
    return;
  }

  // Radial cutoff follows the stick angle, so diagonals are not held to a larger displacement.
  auto apply = [](f64 percentage, const JoystickValue& max, i16& axis_x, i16& axis_y) {
    if (percentage <= 0.0 || percentage >= 1.0) {
      return;
    }
    const f64 deadzone_x = max.first * percentage;
    const f64 deadzone_y = max.second * percentage;
    const f64 theta = std::atan2(static_cast<f64>(axis_y), static_cast<f64>(axis_x));
    const f64 cutoff_x = std::cos(theta) * deadzone_x;
    const f64 cutoff_y = std::sin(theta) * deadzone_y;
    if (axis_x > -cutoff_x && axis_x < cutoff_x) {
      axis_x = 0;
    }
    if (axis_y > -cutoff_y && axis_y < cutoff_y) {
      axis_y = 0;
    }
  };

  const auto& maxima = user_max_joystick_value_[user];
  i16 lx = gamepad.thumb_lx;
  i16 ly = gamepad.thumb_ly;
  apply(REXCVAR_GET(cvars::left_stick_deadzone), maxima.first, lx, ly);
  gamepad.thumb_lx = lx;
  gamepad.thumb_ly = ly;

  i16 rx = gamepad.thumb_rx;
  i16 ry = gamepad.thumb_ry;
  apply(REXCVAR_GET(cvars::right_stick_deadzone), maxima.second, rx, ry);
  gamepad.thumb_rx = rx;
  gamepad.thumb_ry = ry;
}

Result<void> Hub::SetVibration(u32 user, const X_INPUT_VIBRATION& vibration) {
  REX_PROFILE_SCOPE("hid");
  std::lock_guard lock(mutex_);
  if (!assignment_) {
    return std::unexpected(NotConnected());
  }

  RefreshDevicesLocked();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user, ids);

  X_INPUT_VIBRATION modified = vibration;
  if (!REXCVAR_GET(cvars::vibration)) {
    modified.left_motor_speed = 0;
    modified.right_motor_speed = 0;
  }

  // Only pads decide the result, since synthetic devices accept any vibration.
  bool any_pad = false;
  bool any_synthetic = false;
  bool pad_rumbled = false;
  rex::Error pad_error = NotConnected();
  for (DeviceId id : ids) {
    Driver* driver = DriverForDeviceLocked(id);
    const DeviceInfo* info = DeviceInfoForLocked(id);
    if (!driver || !info) {
      continue;
    }
    auto result = driver->SetDeviceVibration(id, modified);
    if (info->synthetic) {
      any_synthetic = true;
      continue;
    }
    any_pad = true;
    if (result) {
      pad_rumbled = true;
    } else {
      pad_error = result.error();
    }
  }
  if (any_pad) {
    if (pad_rumbled) {
      return Ok();
    }
    return std::unexpected(pad_error);
  }
  if (any_synthetic) {
    return Ok();
  }
  return std::unexpected(NotConnected());
}

Result<X_INPUT_KEYSTROKE> Hub::GetKeystroke(u32 user, u32 flags) {
  REX_PROFILE_SCOPE("hid");
  std::lock_guard lock(mutex_);
  if (!assignment_) {
    return std::unexpected(NotConnected());
  }

  RefreshDevicesLocked();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user, ids);

  bool any_connected = false;
  for (DeviceId id : ids) {
    Driver* driver = DriverForDeviceLocked(id);
    if (!driver) {
      continue;
    }
    auto keystroke = driver->GetDeviceKeystroke(id, flags);
    if (keystroke) {
      keystroke->user_index = static_cast<u8>(user);
      return keystroke;
    }
    if (CodeOf(keystroke.error()) != HIDError::kNotConnected) {
      any_connected = true;
    }
  }
  return std::unexpected(any_connected ? BuildError(HIDError::kEmpty, "no keystroke is queued")
                                       : NotConnected());
}

void Hub::SetConnectionListener(rex::MoveOnlyFunction<void(u32, bool)> listener) {
  std::lock_guard lock(mutex_);
  connection_listener_ = std::move(listener);
}

void Hub::SetHost(IInputHost* host) {
  IInputHost* old = host_.load(std::memory_order_acquire);
  if (old && old != host && applied_mode_ != PointerMode::kFree) {
    old->ApplyPointerMode(PointerMode::kFree);
  }
  applied_mode_ = PointerMode::kFree;
  relative_ = false;
  host_.store(host, std::memory_order_release);
  capture_.store(!host || host->GetCaptureActive(), std::memory_order_relaxed);
  {
    std::lock_guard lock(mutex_);
    for (auto& driver : drivers_) {
      driver->AttachHost(host);
    }
  }
  ApplyPointerModeFromUIThread();
}

void Hub::OnEvent(const InputEvent& event) {
  InputEvent forwarded = event;
  switch (event.kind) {
    case InputEvent::Kind::kKeyDown:
      SetKey(event.key, true);
      break;
    case InputEvent::Kind::kKeyUp:
      SetKey(event.key, false);
      break;
    case InputEvent::Kind::kButtonDown:
      buttons_.fetch_or(ButtonBit(event.button), std::memory_order_relaxed);
      break;
    case InputEvent::Kind::kButtonUp:
      buttons_.fetch_and(~ButtonBit(event.button), std::memory_order_relaxed);
      break;
    case InputEvent::Kind::kMotion:
      TrackCapture();
      forwarded = TrackMotion(event);
      break;
    case InputEvent::Kind::kWheel:
      wheel_accum_.fetch_add(event.wheel, std::memory_order_relaxed);
      break;
    case InputEvent::Kind::kFocusGained:
      focused_.store(true, std::memory_order_relaxed);
      break;
    case InputEvent::Kind::kFocusLost:
      ResetOnFocusLoss();
      break;
    case InputEvent::Kind::kResize:
      width_.store(event.width, std::memory_order_relaxed);
      height_.store(event.height, std::memory_order_relaxed);
      break;
    case InputEvent::Kind::kKeyChar:
      break;
  }
  {
    std::lock_guard lock(mutex_);
    for (auto& driver : drivers_) {
      driver->OnEvent(forwarded);
    }
  }
  if (event.kind == InputEvent::Kind::kFocusGained || event.kind == InputEvent::Kind::kFocusLost) {
    ApplyPointerModeFromUIThread();
  }
}

void Hub::SetKey(VirtualKey key, bool down) {
  const auto index = static_cast<u16>(key);
  if (index >= 256) {
    return;
  }
  const u64 bit = u64{1} << (index & 63);
  if (down) {
    keys_[index >> 6].fetch_or(bit, std::memory_order_relaxed);
  } else {
    keys_[index >> 6].fetch_and(~bit, std::memory_order_relaxed);
  }
}

InputEvent Hub::TrackMotion(const InputEvent& event) {
  InputEvent out = event;
  if (applied_mode_ == PointerMode::kLocked) {
    if (!relative_) {
      out.dx = event.x - lock_x_;
      out.dy = event.y - lock_y_;
      lock_x_ = event.x;
      lock_y_ = event.y;
      Recenter(event.x, event.y);
    }
    motion_dx_.fetch_add(out.dx, std::memory_order_relaxed);
    motion_dy_.fetch_add(out.dy, std::memory_order_relaxed);
    return out;
  }
  const f32 previous_x = x_.exchange(event.x, std::memory_order_relaxed);
  const f32 previous_y = y_.exchange(event.y, std::memory_order_relaxed);
  const bool had_position = has_position_.exchange(true, std::memory_order_relaxed);
  out.dx = had_position ? event.x - previous_x : 0.0f;
  out.dy = had_position ? event.y - previous_y : 0.0f;
  if (!had_position || out.dx != 0.0f || out.dy != 0.0f) {
    moved_.store(true, std::memory_order_relaxed);
  }
  return out;
}

void Hub::Recenter(f32 x, f32 y) {
  IInputHost* host = host_.load(std::memory_order_acquire);
  const auto width = static_cast<f32>(width_.load(std::memory_order_relaxed));
  const auto height = static_cast<f32>(height_.load(std::memory_order_relaxed));
  if (!host || width <= 0.0f || height <= 0.0f) {
    return;
  }
  if (std::abs(x - width / 2) < width / 4 && std::abs(y - height / 2) < height / 4) {
    return;
  }
  if (auto center = host->WarpPointerToCenter()) {
    lock_x_ = center->x;
    lock_y_ = center->y;
  }
}

void Hub::ResetOnFocusLoss() {
  focused_.store(false, std::memory_order_relaxed);
  for (auto& word : keys_) {
    word.store(0, std::memory_order_relaxed);
  }
  buttons_.store(0, std::memory_order_relaxed);
  motion_dx_.store(0.0f, std::memory_order_relaxed);
  motion_dy_.store(0.0f, std::memory_order_relaxed);
  wheel_accum_.store(0, std::memory_order_relaxed);
  wheel_taken_.store(0, std::memory_order_relaxed);
  moved_.store(false, std::memory_order_relaxed);
  has_position_.store(false, std::memory_order_relaxed);
}

PointerMode Hub::GetEffectiveMode() const {
  if (!focused_.load(std::memory_order_relaxed) || !capture_.load(std::memory_order_relaxed)) {
    return PointerMode::kFree;
  }
  if (keyboard_mouse_->WantsPointerLock()) {
    return PointerMode::kLocked;
  }
  return requested_mode_.load(std::memory_order_relaxed);
}

void Hub::ApplyPointerModeFromUIThread() {
  IInputHost* host = host_.load(std::memory_order_acquire);
  if (!host) {
    return;
  }
  const PointerMode want = GetEffectiveMode();
  if (want == applied_mode_) {
    return;
  }
  relative_ = host->ApplyPointerMode(want);
  if (want == PointerMode::kLocked) {
    lock_x_ = x_.load(std::memory_order_relaxed);
    lock_y_ = y_.load(std::memory_order_relaxed);
  }
  motion_dx_.store(0.0f, std::memory_order_relaxed);
  motion_dy_.store(0.0f, std::memory_order_relaxed);
  applied_mode_ = want;
}

void Hub::RefreshPointerMode() {
  IInputHost* host = host_.load(std::memory_order_acquire);
  if (!host || pointer_update_queued_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  const bool queued = host->Post([this] {
    pointer_update_queued_.store(false, std::memory_order_release);
    ApplyPointerModeFromUIThread();
  });
  if (!queued) {
    pointer_update_queued_.store(false, std::memory_order_release);
  }
}

void Hub::TrackCapture() {
  IInputHost* host = host_.load(std::memory_order_acquire);
  const bool capture = !host || host->GetCaptureActive();
  if (capture_.exchange(capture, std::memory_order_relaxed) != capture) {
    RefreshPointerMode();
  }
}

void Hub::SetPointerMode(PointerMode mode) {
  requested_mode_.store(mode, std::memory_order_relaxed);
  RefreshPointerMode();
}

PointerMode Hub::GetPointerMode() const {
  return requested_mode_.load(std::memory_order_relaxed);
}

bool Hub::IsKeyDown(VirtualKey key) const {
  const auto index = static_cast<u16>(key);
  if (index >= 256) {
    return false;
  }
  return (keys_[index >> 6].load(std::memory_order_relaxed) >> (index & 63)) & 1;
}

bool Hub::IsAnyKeyDown() const {
  return std::ranges::any_of(keys_,
                             [](const auto& word) { return word.load(std::memory_order_relaxed); });
}

KeyModifiers Hub::GetModifiers() const {
  KeyModifiers mods = KeyModifiers::kNone;
  if (IsKeyDown(VirtualKey::kShift)) {
    mods |= KeyModifiers::kShift;
  }
  if (IsKeyDown(VirtualKey::kControl)) {
    mods |= KeyModifiers::kCtrl;
  }
  if (IsKeyDown(VirtualKey::kMenu)) {
    mods |= KeyModifiers::kAlt;
  }
  return mods;
}

bool Hub::GetFocused() const {
  return focused_.load(std::memory_order_relaxed);
}

PointerState Hub::GetPointer() const {
  return {.x = x_.load(std::memory_order_relaxed),
          .y = y_.load(std::memory_order_relaxed),
          .window_width = width_.load(std::memory_order_relaxed),
          .window_height = height_.load(std::memory_order_relaxed),
          .has_position = has_position_.load(std::memory_order_relaxed)};
}

bool Hub::IsButtonDown(MouseButton button) const {
  return (buttons_.load(std::memory_order_relaxed) & ButtonBit(button)) != 0;
}

bool Hub::IsAnyButtonDown() const {
  return buttons_.load(std::memory_order_relaxed) != 0;
}

bool Hub::TakePointerMoved() {
  return moved_.exchange(false, std::memory_order_relaxed);
}

PointerDelta Hub::TakeMotion() {
  return {.dx = motion_dx_.exchange(0.0f, std::memory_order_relaxed),
          .dy = motion_dy_.exchange(0.0f, std::memory_order_relaxed)};
}

PointerDelta Hub::PeekMotion() const {
  return {.dx = motion_dx_.load(std::memory_order_relaxed),
          .dy = motion_dy_.load(std::memory_order_relaxed)};
}

i32 Hub::TakeWheel() {
  const i32 detents = wheel_accum_.exchange(0, std::memory_order_relaxed);
  wheel_taken_.store(detents, std::memory_order_relaxed);
  return detents;
}

i32 Hub::GetWheel() const {
  return wheel_taken_.load(std::memory_order_relaxed);
}

}  // namespace rex::hid
