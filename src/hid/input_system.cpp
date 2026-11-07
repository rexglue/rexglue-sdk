#include "hid/input_system.h"

#include <algorithm>
#include <cmath>

#include <rex/chrono.h>
#include <rex/hid/input_keys.h>
#include <rex/logging/log_category.h>
#include <rex/hid/input_driver.h>
#include <rex/hid/pad.h>
#include <rex/utility.h>

#include "hid/binding_set.h"
#include "hid/input_internal.h"
#include "hid/key_capture.h"

namespace rex::hid {

using detail::Device;
using detail::DeviceKey;
using detail::kMaxUsers;

namespace {

Error NotFound(const char* what) {
  return Error(ErrorCategory::kNotFound, what);
}

u32 ButtonBit(hid::MouseButton button) {
  return 1u << static_cast<u32>(button);
}

struct Listener {
  MoveOnlyFunction<void(u32, bool)> fn;
  std::recursive_mutex call;
  bool alive = true;
};

struct ListenerList {
  std::mutex mutex;
  std::vector<std::shared_ptr<Listener>> list;
};

struct ConnectionEvent {
  u32 user;
  bool connected;
};

}  // namespace

struct InputSystem::State {
  explicit State(InputSystem& system)
      : owner(system),
        listeners(std::make_shared<ListenerList>()),
        bindings(detail::kKeyBindDescs, "hid"),
        keyboard_mouse(
            std::make_unique<detail::KeyboardMousePad>(system.GetSettings(), bindings, focused)) {}

  InputSystem& owner;
  std::mutex mutex;
  std::shared_ptr<ListenerList> listeners;

  hid::BindingSet bindings;
  std::unique_ptr<detail::KeyboardMousePad> keyboard_mouse;
  detail::IdlePad idle;
  std::vector<rex::hid::IInputDriver*> sources;
  u32 keyboard_mouse_source = 0;
  u32 idle_source = 0;
  bool started = false;

  std::vector<u64> revisions;
  bool stale = true;
  std::vector<detail::Device> devices;
  std::array<std::vector<detail::DeviceKey>, detail::kMaxUsers> users;
  detail::ActiveDeviceTracker active;
  std::array<detail::KeystrokeSynth, detail::kMaxUsers> synth;
  std::array<bool, detail::kMaxUsers> connected{};
  std::array<u16, detail::kMaxUsers> consumed{};
  std::array<u32, detail::kMaxUsers> packet{};
  std::array<rex::hid::PadState, detail::kMaxUsers> last_state{};
  std::array<u64, detail::kMaxUsers> last_signature{};
  std::atomic<i32> ui_blocks{0};

  std::atomic<bool> focused{true};
  std::atomic<bool> captured{true};
  std::array<std::atomic<u64>, 4> keys{};
  std::atomic<u32> buttons{0};
  std::atomic<f32> x{0.0f};
  std::atomic<f32> y{0.0f};
  std::atomic<u32> width{0};
  std::atomic<u32> height{0};
  std::atomic<bool> has_position{false};
  std::atomic<bool> moved{false};
  std::atomic<f32> motion_dx{0.0f};
  std::atomic<f32> motion_dy{0.0f};
  std::atomic<i32> wheel_accum{0};
  std::atomic<f32> wheel_remainder{0.0f};
  std::atomic<i32> wheel_taken{0};
  std::atomic<hid::PointerMode> requested_mode{hid::PointerMode::kFree};
  std::mutex pointer_mutex;
  MoveOnlyFunction<void()> pointer_listener;
  u64 pointer_serial = 0;

  bool IsActive() const {
    return focused.load(std::memory_order_relaxed) && captured.load(std::memory_order_relaxed);
  }

  void RefreshDevicesLocked(std::vector<ConnectionEvent>& events);
  const detail::Device* FindDeviceLocked(const detail::DeviceKey& key) const;
  bool ChooseDeviceLocked(u32 user, detail::DeviceKey& out) const;
  Result<rex::hid::PadState> ReadRawLocked(u32 user);
  Result<InputSystem::PadReading> ReadUserLocked(u32 user);
  void UpdateConnectionsLocked(std::vector<ConnectionEvent>& events);
  void Fire(const std::vector<ConnectionEvent>& events);
};

const detail::Device* InputSystem::State::FindDeviceLocked(const detail::DeviceKey& key) const {
  for (const detail::Device& device : devices) {
    if (device.key == key) {
      return &device;
    }
  }
  return nullptr;
}

void InputSystem::State::RefreshDevicesLocked(std::vector<ConnectionEvent>& events) {
  bool current = !stale && revisions.size() == sources.size();
  for (size_t i = 0; current && i < sources.size(); ++i) {
    current = revisions[i] == sources[i]->GetDeviceRevision();
  }
  if (current) {
    return;
  }
  revisions.resize(sources.size());
  for (size_t i = 0; i < sources.size(); ++i) {
    revisions[i] = sources[i]->GetDeviceRevision();
  }
  stale = false;

  std::vector<detail::Device> seen;
  std::vector<rex::hid::PadDevice> enumerated;
  for (u32 source = 0; source < sources.size(); ++source) {
    const bool is_keyboard_mouse = source == keyboard_mouse_source;
    const bool is_idle = source == idle_source;
    const bool synthetic = is_keyboard_mouse || is_idle;
    enumerated.clear();
    sources[source]->EnumerateDevices(enumerated);
    for (rex::hid::PadDevice& pad : enumerated) {
      if (!synthetic && std::ranges::any_of(seen, [&](const detail::Device& kept) {
            return !kept.synthetic && detail::IsSamePad(kept.pad.identity, pad.identity);
          })) {
        continue;
      }
      detail::Device device;
      device.key = {source, pad.id};
      device.synthetic = synthetic;
      device.counts =
          is_idle ? false : (is_keyboard_mouse ? !keyboard_mouse->IsPassthrough() : true);
      device.pad = std::move(pad);
      seen.push_back(std::move(device));
    }
  }

  detail::AssignOrdinals(devices, seen);
  for (const detail::Device& old : devices) {
    if (std::ranges::none_of(seen,
                             [&](const detail::Device& device) { return device.key == old.key; })) {
      active.Forget(old.key);
    }
  }
  devices = std::move(seen);
  users = detail::AssignUsers(devices, owner.GetSettings().shared_pads.Get());
  UpdateConnectionsLocked(events);
}

void InputSystem::State::UpdateConnectionsLocked(std::vector<ConnectionEvent>& events) {
  for (u32 user = 0; user < detail::kMaxUsers; ++user) {
    bool now = false;
    for (const detail::DeviceKey& key : users[user]) {
      const detail::Device* device = FindDeviceLocked(key);
      now = now || (device != nullptr && device->counts);
    }
    if (now == connected[user]) {
      continue;
    }
    connected[user] = now;
    if (now) {
      REXHID_INFO("New controller connected to user {}.", user);
    } else {
      REXHID_INFO("Controller disconnected from user {}.", user);
      consumed[user] = 0;
    }
    events.push_back({user, now});
  }
}

bool InputSystem::State::ChooseDeviceLocked(u32 user, detail::DeviceKey& out) const {
  if (user >= detail::kMaxUsers || users[user].empty()) {
    return false;
  }
  detail::DeviceKey chosen;
  if (active.GetActive(user, chosen) &&
      std::ranges::find(users[user], chosen) != users[user].end()) {
    out = chosen;
    return true;
  }
  out = users[user].front();
  return true;
}

Result<rex::hid::PadState> InputSystem::State::ReadRawLocked(u32 user) {
  if (user >= detail::kMaxUsers || users[user].empty()) {
    return std::unexpected(NotFound("no pad is assigned to the user"));
  }
  rex::hid::PadState merged;
  bool any = false;
  for (const detail::DeviceKey& key : users[user]) {
    auto state = sources[key.source]->GetDeviceState(key.id);
    if (!state) {
      continue;
    }
    if (key.source == keyboard_mouse_source && !IsActive()) {
      *state = rex::hid::PadState{};
    }
    active.Observe(user, key, *state);
    if (!any) {
      merged = *state;
      any = true;
    } else {
      detail::MergeInto(merged, *state);
    }
  }
  if (!any) {
    return std::unexpected(NotFound("no device answers for the user"));
  }
  if (!owner.GetSettings().guide_button.Get()) {
    merged.buttons = static_cast<u16>(merged.buttons & ~rex::hid::kPadGuide);
  }
  detail::ApplyDeadzones(merged, owner.GetSettings().left_stick_deadzone.Get(),
                         owner.GetSettings().right_stick_deadzone.Get());
  return merged;
}

Result<InputSystem::PadReading> InputSystem::State::ReadUserLocked(u32 user) {
  auto raw = ReadRawLocked(user);
  if (!raw) {
    return std::unexpected(raw.error());
  }
  rex::hid::PadState state = *raw;
  if (ui_blocks.load() > 0) {
    state = {};
  } else if (consumed[user] != 0) {
    consumed[user] &= state.buttons;
    state.buttons = static_cast<u16>(state.buttons & ~consumed[user]);
  }
  u64 signature = 0xCBF29CE484222325ull;
  for (const detail::DeviceKey& key : users[user]) {
    signature = (signature ^ ((static_cast<u64>(key.source) << 32) ^ static_cast<u64>(key.id))) *
                0x100000001B3ull;
  }
  const rex::hid::PadState& before = last_state[user];
  const bool same = before.buttons == state.buttons && before.left_trigger == state.left_trigger &&
                    before.right_trigger == state.right_trigger &&
                    before.thumb_lx == state.thumb_lx && before.thumb_ly == state.thumb_ly &&
                    before.thumb_rx == state.thumb_rx && before.thumb_ry == state.thumb_ry;
  if (!same || last_signature[user] != signature) {
    ++packet[user];
    last_state[user] = state;
    last_signature[user] = signature;
  }
  return InputSystem::PadReading{packet[user], state};
}

void InputSystem::State::Fire(const std::vector<ConnectionEvent>& events) {
  if (events.empty()) {
    return;
  }
  std::vector<std::shared_ptr<Listener>> snapshot;
  {
    std::lock_guard lock(listeners->mutex);
    snapshot = listeners->list;
  }
  for (const ConnectionEvent& event : events) {
    for (const std::shared_ptr<Listener>& listener : snapshot) {
      std::lock_guard call(listener->call);
      if (listener->alive) {
        listener->fn(event.user, event.connected);
      }
    }
  }
}

rex::hid::InputSettings::InputSettings() : SettingOwner("hid") {}

InputSystem::InputSystem() : state_(std::make_unique<State>(*this)) {}

InputSystem::~InputSystem() = default;

hid::IBindingSet& InputSystem::GetKeyBindings() {
  return state_->bindings;
}

std::unique_ptr<hid::IBindingSet> InputSystem::CreateBindingSet(
    std::span<const hid::BindingConfig> configs, std::string_view scope) {
  return std::make_unique<hid::BindingSet>(configs, scope);
}

std::unique_ptr<hid::IKeyCapture> InputSystem::CreateKeyCapture(hid::KeyCaptureConfig config) {
  return std::make_unique<hid::KeyCapture>(*this, config);
}

Result<hid::KeyChord> InputSystem::ParseKeyChord(std::string_view text) const {
  return hid::ParseKeyChord(text);
}

std::string InputSystem::FormatKeyChord(hid::KeyChord chord) const {
  return hid::KeyChordToString(chord);
}

hid::VirtualKey InputSystem::ParseVirtualKey(std::string_view name) const {
  return ::rex::hid::ParseVirtualKey(name);
}

std::string InputSystem::VirtualKeyToString(hid::VirtualKey key) const {
  return ::rex::hid::VirtualKeyToString(key);
}

Result<hid::InputSource> InputSystem::ParseInputSource(std::string_view text) const {
  return ::rex::hid::ParseInputSource(text);
}

std::string InputSystem::InputSourceToString(const hid::InputSource& source) const {
  return ::rex::hid::InputSourceToString(source);
}

Result<void> InputSystem::RegisterBindings(ISettingsRegistry& settings) {
  State& state = *state_;
  Result<void> registered = state.bindings.Register(settings);
  if (registered) {
    state.keyboard_mouse->RefreshBinds();
  }
  return registered;
}

void InputSystem::UnregisterBindings() {
  state_->bindings.Unregister();
}

Result<void> InputSystem::Start(std::span<rex::hid::IInputDriver* const> selected) {
  State& state = *state_;
  state.keyboard_mouse->RefreshBinds();
  std::lock_guard lock(state.mutex);
  state.sources.assign(selected.begin(), selected.end());
  state.keyboard_mouse_source = static_cast<u32>(state.sources.size());
  state.sources.push_back(state.keyboard_mouse.get());
  state.idle_source = static_cast<u32>(state.sources.size());
  state.sources.push_back(&state.idle);
  state.stale = true;
  state.started = true;
  return {};
}

void InputSystem::Stop() {
  State& state = *state_;
  std::lock_guard lock(state.mutex);
  state.sources.clear();
  state.devices.clear();
  state.users = {};
  state.revisions.clear();
  state.connected = {};
  state.started = false;
}

Result<InputSystem::PadReading> InputSystem::GetState(u32 user) {
  State& state = *state_;
  std::vector<ConnectionEvent> events;
  Result<PadReading> result = std::unexpected(NotFound("input is not started"));
  {
    std::lock_guard lock(state.mutex);
    if (state.started) {
      state.RefreshDevicesLocked(events);
      result = state.ReadUserLocked(user);
    }
  }
  state.Fire(events);
  return result;
}

Result<rex::hid::PadCapabilities> InputSystem::GetCapabilities(u32 user) {
  State& state = *state_;
  std::vector<ConnectionEvent> events;
  Result<rex::hid::PadCapabilities> result =
      std::unexpected(NotFound("no pad is assigned to the user"));
  {
    std::lock_guard lock(state.mutex);
    if (state.started) {
      state.RefreshDevicesLocked(events);
      detail::DeviceKey chosen;
      if (state.ChooseDeviceLocked(user, chosen)) {
        result = state.sources[chosen.source]->GetDeviceCapabilities(chosen.id);
        if (result && !settings_.guide_button.Get()) {
          result->supported.buttons =
              static_cast<u16>(result->supported.buttons & ~rex::hid::kPadGuide);
        }
      }
    }
  }
  state.Fire(events);
  return result;
}

Result<void> InputSystem::SetVibration(u32 user, const rex::hid::PadVibration& motors) {
  State& state = *state_;
  std::vector<ConnectionEvent> events;
  Result<void> result = std::unexpected(NotFound("no pad is assigned to the user"));
  {
    std::lock_guard lock(state.mutex);
    if (state.started) {
      state.RefreshDevicesLocked(events);
      rex::hid::PadVibration applied = motors;
      if (!settings_.vibration.Get()) {
        applied = {};
      }
      bool any_pad = false;
      bool any_synthetic = false;
      bool pad_rumbled = false;
      Error pad_error = NotFound("no pad is assigned to the user");
      if (user < detail::kMaxUsers) {
        for (const detail::DeviceKey& key : state.users[user]) {
          const detail::Device* device = state.FindDeviceLocked(key);
          if (device == nullptr) {
            continue;
          }
          auto set = state.sources[key.source]->SetDeviceVibration(key.id, applied);
          if (device->synthetic) {
            any_synthetic = true;
            continue;
          }
          any_pad = true;
          if (set) {
            pad_rumbled = true;
          } else {
            pad_error = set.error();
          }
        }
      }
      if (any_pad) {
        result = pad_rumbled ? Result<void>{} : Result<void>(std::unexpected(pad_error));
      } else if (any_synthetic) {
        result = {};
      }
    }
  }
  state.Fire(events);
  return result;
}

Result<rex::hid::PadKeystroke> InputSystem::GetKeystroke(u32 user, KeystrokeSource sources) {
  State& state = *state_;
  std::vector<ConnectionEvent> events;
  Result<rex::hid::PadKeystroke> result = std::unexpected(NotFound("no keystroke is waiting"));
  {
    std::lock_guard lock(state.mutex);
    if (state.started) {
      state.RefreshDevicesLocked(events);
      const u32 first = user == kAnyUser ? 0 : user;
      const u32 last = user == kAnyUser ? detail::kMaxUsers : user + 1;
      for (u32 candidate = first; candidate < last && candidate < detail::kMaxUsers && !result;
           ++candidate) {
        const bool has_keyboard =
            std::ranges::any_of(state.users[candidate], [&](const detail::DeviceKey& key) {
              return key.source == state.keyboard_mouse_source;
            });
        if (has_keyboard && HasFlags(sources, KeystrokeSource::kKeyboard)) {
          result = state.keyboard_mouse->TakeKeystroke();
        }
        if (!result && HasFlags(sources, KeystrokeSource::kPad)) {
          auto reading = state.ReadUserLocked(candidate);
          if (reading) {
            result = state.synth[candidate].Next(reading->state, Clock::GetGuestUptimeMillis());
          }
        }
        if (result) {
          result->user = static_cast<u8>(candidate);
        }
      }
    }
  }
  state.Fire(events);
  return result;
}

Result<rex::hid::PadInfo> InputSystem::GetPadInfo(u32 user) {
  State& state = *state_;
  std::vector<ConnectionEvent> events;
  Result<rex::hid::PadInfo> result = std::unexpected(NotFound("no pad is assigned to the user"));
  {
    std::lock_guard lock(state.mutex);
    if (state.started && user < detail::kMaxUsers) {
      state.RefreshDevicesLocked(events);
      detail::DeviceKey chosen;
      const bool has_chosen = state.ChooseDeviceLocked(user, chosen);
      const detail::Device* first_pad = nullptr;
      for (const detail::DeviceKey& key : state.users[user]) {
        const detail::Device* device = state.FindDeviceLocked(key);
        if (device == nullptr || device->synthetic) {
          continue;
        }
        if (has_chosen && key == chosen) {
          first_pad = device;
          break;
        }
        if (first_pad == nullptr) {
          first_pad = device;
        }
      }
      if (first_pad != nullptr) {
        result = first_pad->pad.info;
      }
    }
  }
  state.Fire(events);
  return result;
}

ScopeGuard InputSystem::OnConnectionChange(MoveOnlyFunction<void(u32 user, bool connected)> fn) {
  auto entry = std::make_shared<Listener>();
  entry->fn = std::move(fn);
  std::shared_ptr<ListenerList> listeners = state_->listeners;
  {
    std::lock_guard lock(listeners->mutex);
    listeners->list.push_back(entry);
  }
  return ScopeGuard([listeners, entry] {
    {
      std::lock_guard lock(listeners->mutex);
      std::erase(listeners->list, entry);
    }
    std::lock_guard call(entry->call);
    entry->alive = false;
  });
}

void InputSystem::AcquireUIBlock() {
  state_->ui_blocks.fetch_add(1);
}

void InputSystem::ReleaseUIBlock() {
  State& state = *state_;
  std::vector<ConnectionEvent> events;
  {
    std::lock_guard lock(state.mutex);
    if (state.started) {
      state.RefreshDevicesLocked(events);
      for (u32 user = 0; user < detail::kMaxUsers; ++user) {
        if (auto raw = state.ReadRawLocked(user)) {
          state.consumed[user] |= raw->buttons;
        }
      }
    }
    state.ui_blocks.fetch_sub(1);
  }
  state.Fire(events);
}

void InputSystem::OnInputEvent(hid::InputEvent& event) {
  using Type = hid::InputEvent::Type;
  State& state = *state_;
  hid::InputEvent forwarded = event;
  switch (event.type) {
    case Type::kKeyDown:
    case Type::kKeyUp: {
      const auto index = static_cast<u16>(event.key);
      if (index < 256) {
        const u64 bit = u64{1} << (index & 63);
        if (event.type == Type::kKeyDown) {
          state.keys[index >> 6].fetch_or(bit, std::memory_order_relaxed);
        } else {
          state.keys[index >> 6].fetch_and(~bit, std::memory_order_relaxed);
        }
      }
      break;
    }
    case Type::kButtonDown:
      state.buttons.fetch_or(ButtonBit(event.button), std::memory_order_relaxed);
      break;
    case Type::kButtonUp:
      state.buttons.fetch_and(~ButtonBit(event.button), std::memory_order_relaxed);
      break;
    case Type::kMotion: {
      const bool locked =
          state.focused.load(std::memory_order_relaxed) &&
          state.captured.load(std::memory_order_relaxed) &&
          (state.keyboard_mouse->IsPointerLockWanted() ||
           state.requested_mode.load(std::memory_order_relaxed) == hid::PointerMode::kLocked);
      if (locked) {
        state.motion_dx.fetch_add(event.dx, std::memory_order_relaxed);
        state.motion_dy.fetch_add(event.dy, std::memory_order_relaxed);
      } else {
        const f32 previous_x = state.x.exchange(event.x, std::memory_order_relaxed);
        const f32 previous_y = state.y.exchange(event.y, std::memory_order_relaxed);
        const bool had_position = state.has_position.exchange(true, std::memory_order_relaxed);
        forwarded.dx = had_position ? event.x - previous_x : 0.0f;
        forwarded.dy = had_position ? event.y - previous_y : 0.0f;
        if (!had_position || forwarded.dx != 0.0f || forwarded.dy != 0.0f) {
          state.moved.store(true, std::memory_order_relaxed);
        }
      }
      break;
    }
    case Type::kWheel: {
      f32 remainder = state.wheel_remainder.load(std::memory_order_relaxed);
      f32 whole = 0.0f;
      f32 next = 0.0f;
      do {
        const f32 total = remainder + event.scroll_y;
        whole = std::trunc(total);
        next = total - whole;
      } while (
          !state.wheel_remainder.compare_exchange_weak(remainder, next, std::memory_order_relaxed));
      state.wheel_accum.fetch_add(static_cast<i32>(whole), std::memory_order_relaxed);
      break;
    }
    case Type::kFocusGained:
      state.focused.store(true, std::memory_order_relaxed);
      break;
    case Type::kFocusLost:
      state.focused.store(false, std::memory_order_relaxed);
      for (auto& word : state.keys) {
        word.store(0, std::memory_order_relaxed);
      }
      state.buttons.store(0, std::memory_order_relaxed);
      state.motion_dx.store(0.0f, std::memory_order_relaxed);
      state.motion_dy.store(0.0f, std::memory_order_relaxed);
      state.wheel_accum.store(0, std::memory_order_relaxed);
      state.wheel_remainder.store(0.0f, std::memory_order_relaxed);
      state.wheel_taken.store(0, std::memory_order_relaxed);
      state.moved.store(false, std::memory_order_relaxed);
      state.has_position.store(false, std::memory_order_relaxed);
      break;
    case Type::kResize:
      state.width.store(event.width, std::memory_order_relaxed);
      state.height.store(event.height, std::memory_order_relaxed);
      break;
    case Type::kKeyChar:
      break;
  }
  state.keyboard_mouse->OnEvent(forwarded);
}

void InputSystem::SetInputCaptured(bool captured) {
  state_->captured.store(captured, std::memory_order_relaxed);
}

bool InputSystem::IsKeyDown(hid::VirtualKey key) const {
  const auto index = static_cast<u16>(key);
  if (index >= 256) {
    return false;
  }
  return (state_->keys[index >> 6].load(std::memory_order_relaxed) >> (index & 63)) & 1;
}

bool InputSystem::IsAnyKeyDown() const {
  return std::ranges::any_of(
      state_->keys, [](const auto& word) { return word.load(std::memory_order_relaxed) != 0; });
}

hid::KeyModifiers InputSystem::GetModifiers() const {
  hid::KeyModifiers mods = hid::KeyModifiers::kNone;
  if (IsKeyDown(hid::VirtualKey::kShift)) {
    mods |= hid::KeyModifiers::kShift;
  }
  if (IsKeyDown(hid::VirtualKey::kControl)) {
    mods |= hid::KeyModifiers::kCtrl;
  }
  if (IsKeyDown(hid::VirtualKey::kMenu)) {
    mods |= hid::KeyModifiers::kAlt;
  }
  return mods;
}

hid::PointerState InputSystem::GetPointer() const {
  const State& state = *state_;
  return {.x = state.x.load(std::memory_order_relaxed),
          .y = state.y.load(std::memory_order_relaxed),
          .window_width = state.width.load(std::memory_order_relaxed),
          .window_height = state.height.load(std::memory_order_relaxed),
          .has_position = state.has_position.load(std::memory_order_relaxed)};
}

bool InputSystem::IsButtonDown(hid::MouseButton button) const {
  return (state_->buttons.load(std::memory_order_relaxed) & ButtonBit(button)) != 0;
}

bool InputSystem::IsAnyButtonDown() const {
  return state_->buttons.load(std::memory_order_relaxed) != 0;
}

bool InputSystem::TakePointerMoved() {
  return state_->moved.exchange(false, std::memory_order_relaxed);
}

hid::PointerDelta InputSystem::TakeMotion() {
  return {.dx = state_->motion_dx.exchange(0.0f, std::memory_order_relaxed),
          .dy = state_->motion_dy.exchange(0.0f, std::memory_order_relaxed)};
}

hid::PointerDelta InputSystem::PeekMotion() const {
  return {.dx = state_->motion_dx.load(std::memory_order_relaxed),
          .dy = state_->motion_dy.load(std::memory_order_relaxed)};
}

i32 InputSystem::TakeWheel() {
  const i32 detents = state_->wheel_accum.exchange(0, std::memory_order_relaxed);
  state_->wheel_taken.store(detents, std::memory_order_relaxed);
  return detents;
}

i32 InputSystem::GetWheel() const {
  return state_->wheel_taken.load(std::memory_order_relaxed);
}

void InputSystem::SetPointerMode(hid::PointerMode mode) {
  state_->requested_mode.store(mode, std::memory_order_relaxed);
  std::lock_guard lock(state_->pointer_mutex);
  if (state_->pointer_listener) {
    state_->pointer_listener();
  }
}

hid::PointerMode InputSystem::GetPointerModeToApply() const {
  const State& state = *state_;
  if (!state.IsActive()) {
    return hid::PointerMode::kFree;
  }
  if (state.keyboard_mouse->IsPointerLockWanted()) {
    return hid::PointerMode::kLocked;
  }
  return state.requested_mode.load(std::memory_order_relaxed);
}

ScopeGuard InputSystem::SetPointerModeListener(MoveOnlyFunction<void()> listener) {
  State* state = state_.get();
  u64 serial = 0;
  {
    std::lock_guard lock(state->pointer_mutex);
    state->pointer_listener = std::move(listener);
    serial = ++state->pointer_serial;
  }
  return ScopeGuard([state, serial] {
    std::lock_guard lock(state->pointer_mutex);
    if (state->pointer_serial == serial) {
      state->pointer_listener = {};
    }
  });
}

}  // namespace rex::hid
