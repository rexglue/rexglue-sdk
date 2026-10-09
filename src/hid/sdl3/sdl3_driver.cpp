/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <string_view>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/hid/virtual_key.h>
#include <rex/platform/file.h>

#include "hid/common/cvar_table.h"
#include "hid/common/x_result.h"
#include "hid/sdl3/sdl3_driver.h"

namespace rex::hid::sdl3 {

namespace {

/** SDL clamps rumble to SDL_MAX_RUMBLE_DURATION_MS, which is not a public constant. */
constexpr u32 kRumbleDurationMs = 0xFFFF;

const char* JoystickTypeName(SDL_JoystickType t) {
  switch (t) {
    case SDL_JOYSTICK_TYPE_UNKNOWN:
      return "Unknown";
    case SDL_JOYSTICK_TYPE_GAMEPAD:
      return "Gamepad";
    case SDL_JOYSTICK_TYPE_WHEEL:
      return "Wheel";
    case SDL_JOYSTICK_TYPE_ARCADE_STICK:
      return "ArcadeStick";
    case SDL_JOYSTICK_TYPE_FLIGHT_STICK:
      return "FlightStick";
    case SDL_JOYSTICK_TYPE_DANCE_PAD:
      return "DancePad";
    case SDL_JOYSTICK_TYPE_GUITAR:
      return "Guitar";
    case SDL_JOYSTICK_TYPE_DRUM_KIT:
      return "DrumKit";
    case SDL_JOYSTICK_TYPE_ARCADE_PAD:
      return "ArcadePad";
    case SDL_JOYSTICK_TYPE_THROTTLE:
      return "Throttle";
    default:
      return "?";
  }
}

const char* XInputSubTypeName(u8 s) {
  switch (s) {
    case XINPUT_DEVSUBTYPE_GAMEPAD:
      return "GAMEPAD";
    case XINPUT_DEVSUBTYPE_WHEEL:
      return "WHEEL";
    case XINPUT_DEVSUBTYPE_ARCADE_STICK:
      return "ARCADE_STICK";
    case XINPUT_DEVSUBTYPE_FLIGHT_STICK:
      return "FLIGHT_STICK";
    case XINPUT_DEVSUBTYPE_DANCE_PAD:
      return "DANCE_PAD";
    case XINPUT_DEVSUBTYPE_GUITAR:
      return "GUITAR";
    case XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE:
      return "GUITAR_ALTERNATE";
    case XINPUT_DEVSUBTYPE_DRUM_KIT:
      return "DRUM_KIT";
    case XINPUT_DEVSUBTYPE_GUITAR_BASS:
      return "GUITAR_BASS";
    case XINPUT_DEVSUBTYPE_ARCADE_PAD:
      return "ARCADE_PAD";
    default:
      return "?";
  }
}

/**
 * Reports whether a name contains a needle, ignoring case.
 *
 * @param name    Device name, may be null.
 * @param needle  Lowercase ASCII text to find.
 * @return True when the name contains the needle.
 */
bool NameContainsCI(const char* name, std::string_view needle) {
  if (!name || needle.empty()) {
    return false;
  }
  const std::string_view haystack(name);
  return std::search(
             haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == static_cast<unsigned char>(b);
             }) != haystack.end();
}

/**
 * Maps an SDL joystick type to an XINPUT_DEVSUBTYPE_* value. SDL_JoystickType numbering
 * diverges from XINPUT_DEVSUBTYPE_* past value 6, so it cannot be cast.
 *
 * @param t     SDL joystick type.
 * @param name  Device name, used to refine gamepad types.
 * @return XINPUT_DEVSUBTYPE_* value.
 */
u8 SdlTypeToXInputSubType(SDL_JoystickType t, const char* name) {
  switch (t) {
    case SDL_JOYSTICK_TYPE_GAMEPAD:
      // SDL's XInput backend puts the SubType in the device name, such as "XInput Guitar #1".
      if (NameContainsCI(name, "guitar")) {
        return XINPUT_DEVSUBTYPE_GUITAR;
      }
      if (NameContainsCI(name, "drum")) {
        return XINPUT_DEVSUBTYPE_DRUM_KIT;
      }
      if (NameContainsCI(name, "wheel")) {
        return XINPUT_DEVSUBTYPE_WHEEL;
      }
      if (NameContainsCI(name, "dancepad") || NameContainsCI(name, "dance pad")) {
        return XINPUT_DEVSUBTYPE_DANCE_PAD;
      }
      if (NameContainsCI(name, "flightstick") || NameContainsCI(name, "flight stick") ||
          NameContainsCI(name, "hotas")) {
        return XINPUT_DEVSUBTYPE_FLIGHT_STICK;
      }
      if (NameContainsCI(name, "arcadepad") || NameContainsCI(name, "arcade pad")) {
        return XINPUT_DEVSUBTYPE_ARCADE_PAD;
      }
      if (NameContainsCI(name, "arcade")) {
        return XINPUT_DEVSUBTYPE_ARCADE_STICK;
      }
      return XINPUT_DEVSUBTYPE_GAMEPAD;
    case SDL_JOYSTICK_TYPE_WHEEL:
      return XINPUT_DEVSUBTYPE_WHEEL;
    case SDL_JOYSTICK_TYPE_ARCADE_STICK:
      return XINPUT_DEVSUBTYPE_ARCADE_STICK;
    case SDL_JOYSTICK_TYPE_FLIGHT_STICK:
      return XINPUT_DEVSUBTYPE_FLIGHT_STICK;
    case SDL_JOYSTICK_TYPE_DANCE_PAD:
      return XINPUT_DEVSUBTYPE_DANCE_PAD;
    case SDL_JOYSTICK_TYPE_GUITAR:
      return XINPUT_DEVSUBTYPE_GUITAR;
    case SDL_JOYSTICK_TYPE_DRUM_KIT:
      return XINPUT_DEVSUBTYPE_DRUM_KIT;
    case SDL_JOYSTICK_TYPE_ARCADE_PAD:
      return XINPUT_DEVSUBTYPE_ARCADE_PAD;
    default:
      return XINPUT_DEVSUBTYPE_GAMEPAD;
  }
}

u8 GamepadSubType(SDL_Gamepad* gamepad) {
  return SdlTypeToXInputSubType(SDL_GetJoystickType(SDL_GetGamepadJoystick(gamepad)),
                                SDL_GetGamepadName(gamepad));
}

}  // namespace

SDL3Driver::SDL3Driver(std::filesystem::path mappings_file)
    : sdl_events_initialized_(false),
      SDL_Gamepad_initialized_(false),
      mappings_file_(std::move(mappings_file)),
      sdl_events_unflushed_(0),
      sdl_pumpevents_queued_(false),
      controllers_(),
      controllers_mutex_() {}

SDL3Driver::~SDL3Driver() {}

Result<void> SDL3Driver::Setup() {
  if (!TestSDLVersion()) {
    return Err(ErrorCategory::Platform, "SDL3 is older than the gamepad API needs");
  }

  return Ok();
}

void SDL3Driver::OnHostAttached(IInputHost* host) {
  if (host && !attached_) {
    attached_ = true;
    host->Post([this]() {
      if (!attached_) {
        return;
      }
      if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
        REXLOG_ERROR(hid, "SDL: Failed to init events subsystem: {}", SDL_GetError());
        return;
      }
      sdl_events_initialized_ = true;
      pending_events_.reserve(64);

      // An event watch is notified even when the event queue is full.
      SDL_AddEventWatch(EventWatch, this);

      if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        REXLOG_ERROR(hid, "SDL: Failed to init gamecontroller subsystem: {}", SDL_GetError());
        return;
      }
      SDL_Gamepad_initialized_ = true;

      if (!mappings_file_.empty()) {
        auto mappings_path = mappings_file_.is_relative()
                                 ? rex::platform::GetExecutableFolder() / mappings_file_
                                 : mappings_file_;
        if (!std::filesystem::exists(mappings_path)) {
          REXLOG_WARN(hid, "SDL GameControllerDB: file '{}' does not exist.",
                      mappings_path.string());
        } else {
          auto mappings_result = SDL_AddGamepadMappingsFromFile(mappings_path.string().c_str());
          if (mappings_result < 0) {
            REXLOG_ERROR(hid, "SDL GameControllerDB: error loading file '{}': {}.",
                         mappings_path.string(), SDL_GetError());
          } else {
            REXLOG_INFO(hid, "SDL GameControllerDB: loaded {} mappings.", mappings_result);
          }
        }
      }
      REXLOG_INFO(hid, "SDL input driver initialized successfully");
    });
    return;
  }
  if (!host && attached_) {
    for (auto& controller : controllers_) {
      SDL_CloseGamepad(controller.sdl);
    }
    controllers_.clear();
    if (SDL_Gamepad_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
      SDL_Gamepad_initialized_ = false;
    }
    if (sdl_events_initialized_) {
      SDL_RemoveEventWatch(EventWatch, this);
      SDL_QuitSubSystem(SDL_INIT_EVENTS);
      sdl_events_initialized_ = false;
    }
    attached_ = false;
  }
}

bool SDL3Driver::EventWatch(void* userdata, SDL_Event* event) {
  if (!userdata || !event) {
    REX_ASSERT_ALWAYS();
    return false;
  }

  const auto type = event->type;
  if (type < SDL_EVENT_JOYSTICK_AXIS_MOTION || type >= SDL_EVENT_FINGER_DOWN) {
    return false;
  }

  static_cast<SDL3Driver*>(userdata)->HandleEvent(*event);
  return false;
}

void SDL3Driver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  // Polling can start before the window exists and the subsystems are up.
  if (!sdl_events_initialized_ || !SDL_Gamepad_initialized_) {
    return;
  }

  auto guard = DrainAndLock();

  for (const auto& controller : controllers_) {
    DeviceInfo info;
    info.id = controller.id;
    const char* name = SDL_GetGamepadName(controller.sdl);
    info.name = name ? name : "";
    char guid_text[33] = {};
    SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(controller.sdl)), guid_text,
                     static_cast<int>(sizeof(guid_text)));
    info.guid = guid_text;
    info.subtype = GamepadSubType(controller.sdl);
    info.synthetic = false;
    out.push_back(info);
  }
}

Result<X_INPUT_STATE> SDL3Driver::GetDeviceState(DeviceId id) {
  X_INPUT_STATE state{};
  return ToResult(ReadDeviceState(id, &state), state);
}

Result<X_INPUT_CAPABILITIES> SDL3Driver::GetDeviceCapabilities(DeviceId id,
                                                               [[maybe_unused]] u32 flags) {
  X_INPUT_CAPABILITIES caps{};
  return ToResult(ReadDeviceCapabilities(id, &caps), caps);
}

Result<void> SDL3Driver::SetDeviceVibration(DeviceId id, const X_INPUT_VIBRATION& vibration) {
  X_INPUT_VIBRATION copy = vibration;
  return ToResult(WriteDeviceVibration(id, &copy));
}

Result<X_INPUT_KEYSTROKE> SDL3Driver::GetDeviceKeystroke(DeviceId id, [[maybe_unused]] u32 flags) {
  X_INPUT_KEYSTROKE keystroke{};
  return ToResult(ReadDeviceKeystroke(id, &keystroke), keystroke);
}

X_RESULT SDL3Driver::ReadDeviceCapabilities(DeviceId id, X_INPUT_CAPABILITIES* out_caps) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (!out_caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Drivers cannot present all information immediately, such as battery, so refresh each call.
  UpdateXCapabilities(*controller);

  *out_caps = controller->caps;

  return X_ERROR_SUCCESS;
}

X_RESULT SDL3Driver::ReadDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);

  auto is_active = this->is_active();

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // packet_number increments by 1 per call however many updates arrived, and on is_active changes.
  if ((is_active != controller->is_active) || (is_active && controller->state_changed)) {
    controller->state.packet_number++;
    controller->is_active = is_active;
    controller->state_changed = false;
  }
  *out_state = controller->state;
  if (!is_active) {
    // Report an untouched pad while inactive. Held buttons stay in state and reappear.
    out_state->gamepad = {};
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SDL3Driver::WriteDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  return SDL_RumbleGamepad(controller->sdl, vibration->left_motor_speed,
                           vibration->right_motor_speed, kRumbleDurationMs)
             ? X_ERROR_SUCCESS
             : X_ERROR_FUNCTION_FAILED;
}

X_RESULT SDL3Driver::ReadDeviceKeystroke(DeviceId id, X_INPUT_KEYSTROKE* out_keystroke) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (!out_keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  static_assert(sizeof(X_INPUT_GAMEPAD::buttons) == 2);
  static constexpr std::array<VirtualKey, 34> kVkLookup = {
      VirtualKey::kXInputPadDpadUp,
      VirtualKey::kXInputPadDpadDown,
      VirtualKey::kXInputPadDpadLeft,
      VirtualKey::kXInputPadDpadRight,
      VirtualKey::kXInputPadStart,
      VirtualKey::kXInputPadBack,
      VirtualKey::kXInputPadLThumbPress,
      VirtualKey::kXInputPadRThumbPress,
      VirtualKey::kXInputPadLShoulder,
      VirtualKey::kXInputPadRShoulder,
      VirtualKey::kNone, /* Guide has no VK */
      VirtualKey::kNone, /* Unknown */
      VirtualKey::kXInputPadA,
      VirtualKey::kXInputPadB,
      VirtualKey::kXInputPadX,
      VirtualKey::kXInputPadY,
      VirtualKey::kXInputPadLTrigger,
      VirtualKey::kXInputPadRTrigger,
      VirtualKey::kXInputPadLThumbUp,
      VirtualKey::kXInputPadLThumbDown,
      VirtualKey::kXInputPadLThumbRight,
      VirtualKey::kXInputPadLThumbLeft,
      VirtualKey::kXInputPadLThumbUpLeft,
      VirtualKey::kXInputPadLThumbUpRight,
      VirtualKey::kXInputPadLThumbDownRight,
      VirtualKey::kXInputPadLThumbDownLeft,
      VirtualKey::kXInputPadRThumbUp,
      VirtualKey::kXInputPadRThumbDown,
      VirtualKey::kXInputPadRThumbRight,
      VirtualKey::kXInputPadRThumbLeft,
      VirtualKey::kXInputPadRThumbUpLeft,
      VirtualKey::kXInputPadRThumbUpRight,
      VirtualKey::kXInputPadRThumbDownRight,
      VirtualKey::kXInputPadRThumbDownLeft,
  };

  auto is_active = this->is_active();

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  auto controller = FindController(id);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Inactive input reads as unpressed, so UP events are sent and DOWN events return when active.
  const u64 curr_butts =
      is_active ? (static_cast<u64>(static_cast<u16>(controller->state.gamepad.buttons)) |
                   AnalogToKeyfield(controller->state.gamepad))
                : static_cast<u64>(0);
  KeystrokeState& last = controller->keystroke;

  auto guest_now = rex::Clock::QueryGuestUptimeMillis();
  static_assert(HID_SDL_REPEAT_DELAY >= HID_SDL_REPEAT_RATE);
  if (last.repeat_state == RepeatState::Waiting &&
      (last.repeat_time + HID_SDL_REPEAT_DELAY < guest_now)) {
    last.repeat_state = RepeatState::Repeating;
  }
  if (last.repeat_state == RepeatState::Repeating &&
      (last.repeat_time + HID_SDL_REPEAT_RATE < guest_now)) {
    last.repeat_time = guest_now;
    VirtualKey vk = kVkLookup.at(last.repeat_butt_idx);
    REX_ASSERT(vk != VirtualKey::kNone);
    out_keystroke->virtual_key = static_cast<u16>(vk);
    out_keystroke->unicode = 0;
    // The hub stamps the title user this device is assigned to.
    out_keystroke->user_index = 0;
    out_keystroke->hid_code = 0;
    out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT;
    return X_ERROR_SUCCESS;
  }

  auto butts_changed = curr_butts ^ last.buttons;
  if (!butts_changed) {
    return X_ERROR_EMPTY;
  }

  // Clear pass first, matching xinput order, so THUMB_UPLEFT is up before THUMB_LEFT is down.
  for (auto [clear_pass, pass] = std::tuple{true, 0}; pass < 2; clear_pass = false, pass++) {
    for (u8 i = 0; i < u8(std::size(kVkLookup)); i++) {
      auto fbutton = static_cast<u64>(1) << i;
      if (!(butts_changed & fbutton)) {
        continue;
      }
      VirtualKey vk = kVkLookup.at(i);
      if (vk == VirtualKey::kNone) {
        continue;
      }

      out_keystroke->virtual_key = static_cast<u16>(vk);
      out_keystroke->unicode = 0;
      out_keystroke->user_index = 0;
      out_keystroke->hid_code = 0;

      bool is_pressed = curr_butts & fbutton;
      if (clear_pass && !is_pressed) {
        out_keystroke->flags = X_INPUT_KEYSTROKE_KEYUP;
        last.buttons &= ~fbutton;
        last.repeat_state = RepeatState::Idle;
        return X_ERROR_SUCCESS;
      }
      if (!clear_pass && is_pressed) {
        out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN;
        last.buttons |= fbutton;
        last.repeat_state = RepeatState::Waiting;
        last.repeat_butt_idx = i;
        last.repeat_time = guest_now;
        return X_ERROR_SUCCESS;
      }
    }
  }
  return X_ERROR_EMPTY;
}

void SDL3Driver::HandleEvent(const SDL_Event& event) {
  assert(SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) < 0xFFFF);

  // SDL's queue is never polled and can grow to 3.5MB, so it is flushed every 64 events.
  if (++sdl_events_unflushed_ > 64) {
    SDL_FlushEvents(SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_FINGER_DOWN - 1);
    sdl_events_unflushed_ = 0;
  }

  std::lock_guard<std::mutex> guard(event_queue_mutex_);
  pending_events_.push_back(event);
}

std::unique_lock<std::mutex> SDL3Driver::DrainAndLock() {
  std::vector<SDL_Event> events;
  {
    std::lock_guard<std::mutex> guard(event_queue_mutex_);
    events.swap(pending_events_);
  }
  std::unique_lock<std::mutex> guard(controllers_mutex_);
  for (const auto& event : events) {
    ProcessEventLocked(event);
  }
  return guard;
}

void SDL3Driver::ProcessEventLocked(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_JOYSTICK_ADDED:
      OnJoystickDeviceAddedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_ADDED:
      OnControllerDeviceAddedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      OnControllerDeviceRemovedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      OnControllerDeviceAxisMotionLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      OnControllerDeviceButtonChangedLocked(event);
      break;
    default:
      break;
  }
}

void SDL3Driver::OnJoystickDeviceAddedLocked(const SDL_Event& event) {
  // Devices SDL cannot promote to a gamepad never reach the gamepad event.
  const SDL_JoystickID id = event.jdevice.which;
  const char* name = SDL_GetJoystickNameForID(id);
  char guid_text[33] = {};
  SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid_text, static_cast<int>(sizeof(guid_text)));
  const bool is_gamepad = SDL_IsGamepad(id);
  REXLOG_DEBUG(hid,
               "SDL JoystickAdded: \"{}\", VendorID(0x{:04X}), ProductID(0x{:04X}), GUID({}), "
               "HasGamepadMapping({})",
               name ? name : "?", SDL_GetJoystickVendorForID(id), SDL_GetJoystickProductForID(id),
               guid_text, is_gamepad);
  if (!is_gamepad) {
    REXLOG_WARN(
        hid,
        "SDL JoystickAdded: \"{}\" has no gamepad mapping, so it will not be usable. Add one to "
        "the file named by Config::hid.mappings_file.",
        name ? name : "?");
  }
}

void SDL3Driver::OnControllerDeviceAddedLocked(const SDL_Event& event) {
  const auto controller = SDL_OpenGamepad(event.gdevice.which);
  if (!controller) {
    REX_ASSERT_ALWAYS();
    return;
  }
  char guid_text[33] = {};
  SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(controller)), guid_text,
                   static_cast<int>(sizeof(guid_text)));
  const SDL_JoystickType joy_type = SDL_GetJoystickType(SDL_GetGamepadJoystick(controller));
  const u8 subtype = GamepadSubType(controller);
  REXLOG_DEBUG(hid,
               "SDL OnControllerDeviceAdded: \"{}\", "
               "JoystickType({}), "
               "GameControllerType({}), "
               "XInputSubType({} = 0x{:02X}), "
               "VendorID(0x{:04X}), "
               "ProductID(0x{:04X}), "
               "GUID({})",
               SDL_GetGamepadName(controller), JoystickTypeName(joy_type),
               static_cast<int>(SDL_GetGamepadType(controller)), XInputSubTypeName(subtype),
               subtype, SDL_GetGamepadVendor(controller), SDL_GetGamepadProduct(controller),
               guid_text);

  // SDL_GetGamepadPlayerIndex is ignored, it is the OS XInput index and not the connection order.
  ControllerState state = {};
  state.sdl = controller;
  state.id = AllocateDeviceId();
  // XInput starts with packet_number = 1.
  state.state_changed = true;
  UpdateXCapabilities(state);
  controllers_.push_back(state);

  const int ordinal = static_cast<int>(controllers_.size()) - 1;
  SDL_SetGamepadPlayerIndex(controller, ordinal);

  REXLOG_DEBUG(hid, "SDL OnControllerDeviceAdded: connection order {}, device {}.", ordinal,
               static_cast<u64>(state.id));
  const char* mapping = SDL_GetGamepadMapping(controller);
  REXLOG_DEBUG(hid, "SDL Controller {}: {}", ordinal, mapping ? mapping : "<none>");
}

void SDL3Driver::OnControllerDeviceRemovedLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gdevice.which);
  if (!idx) {
    REXLOG_WARN(hid, "SDL OnControllerDeviceRemoved: Ignored, instance_id({}) not in use.",
                event.gdevice.which);
    return;
  }
  SDL_Gamepad* removed = controllers_.at(*idx).sdl;
  const char* removed_name = SDL_GetGamepadName(removed);
  char removed_guid[33] = {};
  SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(removed)), removed_guid,
                   static_cast<int>(sizeof(removed_guid)));
  REXLOG_DEBUG(hid, "SDL OnControllerDeviceRemoved: \"{}\", GUID({}).",
               removed_name ? removed_name : "?", removed_guid);
  SDL_CloseGamepad(removed);
  controllers_.erase(controllers_.begin() + static_cast<ptrdiff_t>(*idx));

  // LEDs only, title user assignment does not shift since the hub holds each ordinal.
  for (size_t i = 0; i < controllers_.size(); i++) {
    SDL_SetGamepadPlayerIndex(controllers_[i].sdl, static_cast<int>(i));
  }
  REXLOG_DEBUG(hid, "SDL OnControllerDeviceRemoved: {} device(s) remain.", controllers_.size());
}

void SDL3Driver::OnControllerDeviceAxisMotionLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gaxis.which);
  if (!idx) {
    return;
  }
  auto& pad = controllers_.at(*idx).state.gamepad;
  switch (event.gaxis.axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
      pad.thumb_lx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFTY:
      pad.thumb_ly = ~event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTX:
      pad.thumb_rx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTY:
      pad.thumb_ry = ~event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
      pad.left_trigger = static_cast<u8>(event.gaxis.value >> 7);
      break;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
      pad.right_trigger = static_cast<u8>(event.gaxis.value >> 7);
      break;
    default:
      REX_ASSERT_ALWAYS();
      break;
  }
  controllers_.at(*idx).state_changed = true;
}

void SDL3Driver::OnControllerDeviceButtonChangedLocked(const SDL_Event& event) {
  static constexpr std::array<std::underlying_type<X_INPUT_GAMEPAD_BUTTON>::type, 21>
      xbutton_lookup = {
          X_INPUT_GAMEPAD_A,
          X_INPUT_GAMEPAD_B,
          X_INPUT_GAMEPAD_X,
          X_INPUT_GAMEPAD_Y,
          X_INPUT_GAMEPAD_BACK,
          X_INPUT_GAMEPAD_GUIDE,
          X_INPUT_GAMEPAD_START,
          X_INPUT_GAMEPAD_LEFT_THUMB,
          X_INPUT_GAMEPAD_RIGHT_THUMB,
          X_INPUT_GAMEPAD_LEFT_SHOULDER,
          X_INPUT_GAMEPAD_RIGHT_SHOULDER,
          X_INPUT_GAMEPAD_DPAD_UP,
          X_INPUT_GAMEPAD_DPAD_DOWN,
          X_INPUT_GAMEPAD_DPAD_LEFT,
          X_INPUT_GAMEPAD_DPAD_RIGHT,
          X_INPUT_GAMEPAD_GUIDE,
          X_INPUT_GAMEPAD_Y,
          X_INPUT_GAMEPAD_B,
          X_INPUT_GAMEPAD_X,
          X_INPUT_GAMEPAD_A,
          X_INPUT_GAMEPAD_GUIDE,
      };
  static_assert(SDL_GAMEPAD_BUTTON_SOUTH == 0);
  static_assert(SDL_GAMEPAD_BUTTON_DPAD_RIGHT == 14);

  auto idx = GetControllerIndexFromInstanceID(event.gbutton.which);
  if (!idx) {
    return;
  }
  auto& controller = controllers_.at(*idx);

  u16 xbuttons = controller.state.gamepad.buttons;
  if (event.gbutton.button >= xbutton_lookup.size()) {
    REXLOG_DEBUG(hid, "SDL HID: Unknown button was pressed: {}.", event.gbutton.button);
    return;
  }
  auto xbutton = xbutton_lookup.at(event.gbutton.button);
  if (event.gbutton.down) {
    if (xbutton == X_INPUT_GAMEPAD_GUIDE && !REXCVAR_GET(cvars::guide_button)) {
      return;
    }
    xbuttons |= xbutton;
  } else {
    xbuttons &= ~xbutton;
  }
  controller.state.gamepad.buttons = xbuttons;
  controller.state_changed = true;
}

std::optional<size_t> SDL3Driver::GetControllerIndexFromInstanceID(SDL_JoystickID instance_id) {
  for (size_t i = 0; i < controllers_.size(); i++) {
    auto controller = controllers_.at(i).sdl;
    auto joystick = SDL_GetGamepadJoystick(controller);
    assert(joystick);
    auto joy_instance_id = SDL_GetJoystickID(joystick);
    assert(joy_instance_id >= 0);
    if (joy_instance_id == instance_id) {
      return i;
    }
  }
  return std::nullopt;
}

SDL3Driver::ControllerState* SDL3Driver::FindController(DeviceId id) {
  if (id == DeviceId::kInvalid) {
    return nullptr;
  }
  for (auto& controller : controllers_) {
    if (controller.id == id) {
      return &controller;
    }
  }
  return nullptr;
}

DeviceId SDL3Driver::AllocateDeviceId() {
  return static_cast<DeviceId>(next_device_id_++);
}

bool SDL3Driver::TestSDLVersion() const {
  REXLOG_DEBUG(hid, "SDL: Using version {}.{}.{}", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
               SDL_MICRO_VERSION);
  return true;
}

void SDL3Driver::UpdateXCapabilities(ControllerState& state) {
  assert(state.sdl);
  u16 cap_flags = 0x0;

  SDL_Joystick* joystick = SDL_GetGamepadJoystick(state.sdl);
  if (SDL_GetJoystickConnectionState(joystick) == SDL_JOYSTICK_CONNECTION_WIRELESS) {
    cap_flags |= X_INPUT_CAPS_WIRELESS;
  } else {
    // Backends leaving the connection state UNKNOWN report power.
    const SDL_PowerState power_state = SDL_GetJoystickPowerInfo(joystick, nullptr);
    if (power_state == SDL_POWERSTATE_ON_BATTERY || power_state == SDL_POWERSTATE_CHARGING ||
        power_state == SDL_POWERSTATE_CHARGED) {
      cap_flags |= X_INPUT_CAPS_WIRELESS;
    }
  }

  static constexpr std::array<SDL_GamepadButton, 6> nav_buttons = {
      SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,      SDL_GAMEPAD_BUTTON_DPAD_UP,
      SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
  };
  for (auto it = nav_buttons.begin(); it < nav_buttons.end(); it++) {
    if (!SDL_GamepadHasButton(state.sdl, *it)) {
      cap_flags |= X_INPUT_CAPS_NO_NAVIGATION;
      break;
    }
  }

  auto& c = state.caps;
  c.type = XINPUT_DEVTYPE_GAMEPAD;
  c.sub_type = GamepadSubType(state.sdl);
  c.flags = cap_flags;
  c.gamepad.buttons = 0xF3FF | (REXCVAR_GET(cvars::guide_button) ? X_INPUT_GAMEPAD_GUIDE : 0x0);
  c.gamepad.left_trigger = 0xFF;
  c.gamepad.right_trigger = 0xFF;
  c.gamepad.thumb_lx = static_cast<i16>(0xFFFFu);
  c.gamepad.thumb_ly = static_cast<i16>(0xFFFFu);
  c.gamepad.thumb_rx = static_cast<i16>(0xFFFFu);
  c.gamepad.thumb_ry = static_cast<i16>(0xFFFFu);
  c.vibration.left_motor_speed = 0xFFFFu;
  c.vibration.right_motor_speed = 0xFFFFu;
}

void SDL3Driver::QueueControllerUpdate() {
  bool is_queued = false;
  sdl_pumpevents_queued_.compare_exchange_strong(is_queued, true);
  if (is_queued) {
    return;
  }
  IInputHost* current = host();
  if (!current) {
    sdl_pumpevents_queued_ = false;
    return;
  }
  const bool queued = current->Post([this]() {
    SDL_PumpEvents();
    sdl_pumpevents_queued_ = false;
  });
  if (!queued) {
    sdl_pumpevents_queued_ = false;
  }
}

inline u64 SDL3Driver::AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const {
  u64 f = 0;

  f |= static_cast<u64>(gamepad.left_trigger > HID_SDL_TRIGG_THRES) << 16;
  f |= static_cast<u64>(gamepad.right_trigger > HID_SDL_TRIGG_THRES) << 17;

  auto thumb_x = static_cast<i16>(gamepad.thumb_lx);
  auto thumb_y = static_cast<i16>(gamepad.thumb_ly);
  for (size_t i = 0; i <= 8; i = i + 8) {
    u64 u = thumb_y > HID_SDL_THUMB_THRES;
    u64 d = thumb_y < ~HID_SDL_THUMB_THRES;
    u64 r = thumb_x > HID_SDL_THUMB_THRES;
    u64 l = thumb_x < ~HID_SDL_THUMB_THRES;
    if (u && l) {
      u = l = 0;
      f |= static_cast<u64>(1) << (22 + i);
    }
    if (u && r) {
      u = r = 0;
      f |= static_cast<u64>(1) << (23 + i);
    }
    if (d && r) {
      d = r = 0;
      f |= static_cast<u64>(1) << (24 + i);
    }
    if (d && l) {
      d = l = 0;
      f |= static_cast<u64>(1) << (25 + i);
    }
    f |= u << (18 + i);
    f |= d << (19 + i);
    f |= r << (20 + i);
    f |= l << (21 + i);

    thumb_x = static_cast<i16>(gamepad.thumb_rx);
    thumb_y = static_cast<i16>(gamepad.thumb_ry);
  }
  return f;
}

}  // namespace rex::hid::sdl3
