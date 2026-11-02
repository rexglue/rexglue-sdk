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

#include "hid/sdl3/sdl3_input_driver.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/driver.h>
#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/logging/log.h>
#include <rex/hid/input_driver.h>
#include <rex/hid/pad.h>
#include <rex/platform.h>
#include <rex/platform/file.h>
#include <rex/settings.h>

namespace rex::hid::sdl3 {

namespace {

constexpr u32 kRumbleDurationMs = 0xFFFF;
constexpr size_t kMaxEventsBeforeFlush = 64;

constexpr u8 kSubtypeGamepad = 0x01;
constexpr u8 kSubtypeWheel = 0x02;
constexpr u8 kSubtypeArcadeStick = 0x03;
constexpr u8 kSubtypeFlightStick = 0x04;
constexpr u8 kSubtypeDancePad = 0x05;
constexpr u8 kSubtypeGuitar = 0x06;
constexpr u8 kSubtypeDrumKit = 0x08;
constexpr u8 kSubtypeArcadePad = 0x13;
constexpr u16 kCapsWireless = 0x0002;
constexpr u16 kCapsNoNavigation = 0x0010;
constexpr u16 kSupportedButtons = 0xF3FF | kPadGuide;

Error NotConnected() {
  return Error(ErrorCategory::kNotFound, "the pad disconnected");
}

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

u8 SDLTypeToSubtype(SDL_JoystickType type, const char* name) {
  switch (type) {
    case SDL_JOYSTICK_TYPE_GAMEPAD:
      if (NameContainsCI(name, "guitar")) {
        return kSubtypeGuitar;
      }
      if (NameContainsCI(name, "drum")) {
        return kSubtypeDrumKit;
      }
      if (NameContainsCI(name, "wheel")) {
        return kSubtypeWheel;
      }
      if (NameContainsCI(name, "dancepad") || NameContainsCI(name, "dance pad")) {
        return kSubtypeDancePad;
      }
      if (NameContainsCI(name, "flightstick") || NameContainsCI(name, "flight stick") ||
          NameContainsCI(name, "hotas")) {
        return kSubtypeFlightStick;
      }
      if (NameContainsCI(name, "arcadepad") || NameContainsCI(name, "arcade pad")) {
        return kSubtypeArcadePad;
      }
      if (NameContainsCI(name, "arcade")) {
        return kSubtypeArcadeStick;
      }
      return kSubtypeGamepad;
    case SDL_JOYSTICK_TYPE_WHEEL:
      return kSubtypeWheel;
    case SDL_JOYSTICK_TYPE_ARCADE_STICK:
      return kSubtypeArcadeStick;
    case SDL_JOYSTICK_TYPE_FLIGHT_STICK:
      return kSubtypeFlightStick;
    case SDL_JOYSTICK_TYPE_DANCE_PAD:
      return kSubtypeDancePad;
    case SDL_JOYSTICK_TYPE_GUITAR:
      return kSubtypeGuitar;
    case SDL_JOYSTICK_TYPE_DRUM_KIT:
      return kSubtypeDrumKit;
    case SDL_JOYSTICK_TYPE_ARCADE_PAD:
      return kSubtypeArcadePad;
    default:
      return kSubtypeGamepad;
  }
}

u8 GamepadSubtype(SDL_Gamepad* gamepad) {
  return SDLTypeToSubtype(SDL_GetJoystickType(SDL_GetGamepadJoystick(gamepad)),
                          SDL_GetGamepadName(gamepad));
}

PadType ToPadType(SDL_GamepadType type) {
  switch (type) {
    case SDL_GAMEPAD_TYPE_STANDARD:
      return PadType::kStandard;
    case SDL_GAMEPAD_TYPE_XBOX360:
      return PadType::kX360;
    case SDL_GAMEPAD_TYPE_XBOXONE:
      return PadType::kXOne;
    case SDL_GAMEPAD_TYPE_PS3:
      return PadType::kPS3;
    case SDL_GAMEPAD_TYPE_PS4:
      return PadType::kPS4;
    case SDL_GAMEPAD_TYPE_PS5:
      return PadType::kPS5;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
      return PadType::kSwitchPro;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
      return PadType::kSwitchJoyCon;
    default:
      return PadType::kUnknown;
  }
}

i32 GetXInputSlot([[maybe_unused]] SDL_Gamepad* gamepad) {
#if REX_PLATFORM_WIN32
  constexpr i32 kXInputSlotCount = 4;
  const char* path = SDL_GetGamepadPath(gamepad);
  if (path && std::string_view(path).starts_with("XInput#")) {
    const int index = SDL_GetGamepadPlayerIndex(gamepad);
    if (index >= 0 && index < kXInputSlotCount) {
      return index;
    }
  }
#endif
  return -1;
}

}  // namespace

struct SDL3InputDriver::State {
  struct Controller {
    SDL_Gamepad* sdl = nullptr;
    PadDeviceID id = PadDeviceID::kInvalid;
    PadState state;
    i32 xinput_slot = -1;
  };

  IDriverContext* context = nullptr;
  std::atomic<u64> revision{1};
  std::atomic<bool> running{false};
  std::atomic<bool> pump_queued{false};
  std::atomic<size_t> unflushed{0};

  std::mutex event_mutex;
  std::vector<SDL_Event> pending;

  std::mutex mutex;
  std::vector<Controller> controllers;
  u64 next_id = 1;

  std::atomic<bool> events_up{false};
  std::atomic<bool> gamepad_up{false};

  static bool EventWatch(void* userdata, SDL_Event* event);
  void HandleEvent(const SDL_Event& event);
  std::unique_lock<std::mutex> DrainAndLock();
  void ProcessEventLocked(const SDL_Event& event);
  void OnJoystickAddedLocked(const SDL_Event& event);
  void OnGamepadAddedLocked(const SDL_Event& event);
  void OnGamepadRemovedLocked(const SDL_Event& event);
  void OnAxisLocked(const SDL_Event& event);
  void OnButtonLocked(const SDL_Event& event);
  Controller* FindByInstance(SDL_JoystickID instance);
  Controller* FindByID(PadDeviceID id);
  void QueuePump(const std::shared_ptr<State>& self);
};

bool SDL3InputDriver::State::EventWatch(void* userdata, SDL_Event* event) {
  if (!userdata || !event) {
    return false;
  }
  const auto type = event->type;
  if (type < SDL_EVENT_JOYSTICK_AXIS_MOTION || type >= SDL_EVENT_FINGER_DOWN) {
    return false;
  }
  static_cast<State*>(userdata)->HandleEvent(*event);
  return false;
}

void SDL3InputDriver::State::HandleEvent(const SDL_Event& event) {
  // SDL's queue is never polled and can reach 3.5 MB, so flush it every 64 events.
  if (++unflushed > kMaxEventsBeforeFlush) {
    SDL_FlushEvents(SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_FINGER_DOWN - 1);
    unflushed = 0;
  }
  if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED) {
    ++revision;
  }
  std::lock_guard<std::mutex> guard(event_mutex);
  pending.push_back(event);
}

std::unique_lock<std::mutex> SDL3InputDriver::State::DrainAndLock() {
  std::vector<SDL_Event> events;
  {
    std::lock_guard<std::mutex> guard(event_mutex);
    events.swap(pending);
  }
  std::unique_lock<std::mutex> guard(mutex);
  for (const SDL_Event& event : events) {
    ProcessEventLocked(event);
  }
  return guard;
}

void SDL3InputDriver::State::ProcessEventLocked(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_JOYSTICK_ADDED:
      OnJoystickAddedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_ADDED:
      OnGamepadAddedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      OnGamepadRemovedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      OnAxisLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      OnButtonLocked(event);
      break;
    default:
      break;
  }
}

void SDL3InputDriver::State::OnJoystickAddedLocked(const SDL_Event& event) {
  const SDL_JoystickID id = event.jdevice.which;
  const char* name = SDL_GetJoystickNameForID(id);
  const bool is_gamepad = SDL_IsGamepad(id);
  REXLOG_DEBUG(hid,
               "SDL JoystickAdded: \"{}\", VendorID(0x{:04X}), ProductID(0x{:04X}), mapped({})",
               name ? name : "?", SDL_GetJoystickVendorForID(id), SDL_GetJoystickProductForID(id),
               is_gamepad);
  if (!is_gamepad) {
    REXLOG_WARN(hid,
                "SDL JoystickAdded: \"{}\" has no gamepad mapping, so it will not be usable. Add "
                "one to the file named by hid.sdl.mappings_file.",
                name ? name : "?");
  }
}

void SDL3InputDriver::State::OnGamepadAddedLocked(const SDL_Event& event) {
  SDL_Gamepad* gamepad = SDL_OpenGamepad(event.gdevice.which);
  if (!gamepad) {
    REXLOG_WARN(hid, "SDL: cannot open gamepad: {}", SDL_GetError());
    return;
  }
  Controller controller;
  controller.sdl = gamepad;
  controller.id = static_cast<PadDeviceID>(next_id++);
  controller.xinput_slot = GetXInputSlot(gamepad);
  REXLOG_DEBUG(hid, "SDL GamepadAdded: \"{}\", VendorID(0x{:04X}), ProductID(0x{:04X}), slot {}",
               SDL_GetGamepadName(gamepad), SDL_GetGamepadVendor(gamepad),
               SDL_GetGamepadProduct(gamepad), controller.xinput_slot);
  if (controller.xinput_slot < 0 && SDL_GetGamepadPlayerIndex(gamepad) < 0) {
    i32 free_index = 0;
    while (std::ranges::any_of(controllers, [free_index](const Controller& other) {
      return SDL_GetGamepadPlayerIndex(other.sdl) == free_index;
    })) {
      ++free_index;
    }
    SDL_SetGamepadPlayerIndex(gamepad, free_index);
  }
  controllers.push_back(controller);
}

void SDL3InputDriver::State::OnGamepadRemovedLocked(const SDL_Event& event) {
  Controller* controller = FindByInstance(event.gdevice.which);
  if (!controller) {
    REXLOG_WARN(hid, "SDL GamepadRemoved: ignored, instance {} not in use.", event.gdevice.which);
    return;
  }
  SDL_CloseGamepad(controller->sdl);
  controllers.erase(controllers.begin() + (controller - controllers.data()));
}

void SDL3InputDriver::State::OnAxisLocked(const SDL_Event& event) {
  Controller* controller = FindByInstance(event.gaxis.which);
  if (!controller) {
    return;
  }
  PadState& pad = controller->state;
  switch (event.gaxis.axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
      pad.thumb_lx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFTY:
      pad.thumb_ly = static_cast<i16>(~event.gaxis.value);
      break;
    case SDL_GAMEPAD_AXIS_RIGHTX:
      pad.thumb_rx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTY:
      pad.thumb_ry = static_cast<i16>(~event.gaxis.value);
      break;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
      pad.left_trigger = static_cast<u8>(event.gaxis.value >> 7);
      break;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
      pad.right_trigger = static_cast<u8>(event.gaxis.value >> 7);
      break;
    default:
      break;
  }
}

void SDL3InputDriver::State::OnButtonLocked(const SDL_Event& event) {
  static constexpr std::array<u16, 21> kButtonLookup = {
      kPadA,
      kPadB,
      kPadX,
      kPadY,
      kPadBack,
      kPadGuide,
      kPadStart,
      kPadLeftThumb,
      kPadRightThumb,
      kPadLeftShoulder,
      kPadRightShoulder,
      kPadDPadUp,
      kPadDPadDown,
      kPadDPadLeft,
      kPadDPadRight,
      kPadGuide,
      kPadY,
      kPadB,
      kPadX,
      kPadA,
      kPadGuide,
  };
  static_assert(SDL_GAMEPAD_BUTTON_SOUTH == 0);
  static_assert(SDL_GAMEPAD_BUTTON_DPAD_RIGHT == 14);

  Controller* controller = FindByInstance(event.gbutton.which);
  if (!controller || event.gbutton.button >= kButtonLookup.size()) {
    return;
  }
  const u16 bit = kButtonLookup[event.gbutton.button];
  if (event.gbutton.down) {
    controller->state.buttons = static_cast<u16>(controller->state.buttons | bit);
  } else {
    controller->state.buttons = static_cast<u16>(controller->state.buttons & ~bit);
  }
}

SDL3InputDriver::State::Controller* SDL3InputDriver::State::FindByInstance(
    SDL_JoystickID instance) {
  for (Controller& controller : controllers) {
    if (SDL_GetJoystickID(SDL_GetGamepadJoystick(controller.sdl)) == instance) {
      return &controller;
    }
  }
  return nullptr;
}

SDL3InputDriver::State::Controller* SDL3InputDriver::State::FindByID(PadDeviceID id) {
  for (Controller& controller : controllers) {
    if (controller.id == id) {
      return &controller;
    }
  }
  return nullptr;
}

void SDL3InputDriver::State::QueuePump(const std::shared_ptr<State>& self) {
  if (!running || !events_up || pump_queued.exchange(true)) {
    return;
  }
  context->PostToUIThread([self]() {
    SDL_PumpEvents();
    self->pump_queued = false;
  });
}

SDL3InputDriver::SDL3InputDriver() : SettingOwner("hid.sdl") {}

std::string_view SDL3InputDriver::GetName() const {
  return "sdl3";
}

Result<void> SDL3InputDriver::Initialize(IDriverContext& context) {
  REX_TRY_VOID(context.GetSettings().Register(*this));

  state_ = std::make_shared<State>();
  state_->context = &context;
  state_->running = true;

  std::filesystem::path mappings = mappings_file.Get();
  const Result<void> ran =
      context.RunOnUIThread([state = state_, mappings = std::move(mappings)]() {
        if (!state->running) {
          return;
        }
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
        if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
          REXLOG_ERROR(hid, "SDL: failed to init events subsystem: {}", SDL_GetError());
          return;
        }
        state->events_up = true;
        state->pending.reserve(64);
        SDL_AddEventWatch(&State::EventWatch, state.get());

        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
          REXLOG_ERROR(hid, "SDL: failed to init gamepad subsystem: {}", SDL_GetError());
          return;
        }
        state->gamepad_up = true;
        ++state->revision;

        if (!mappings.empty()) {
          const std::filesystem::path path =
              mappings.is_relative() ? rex::platform::GetExecutableFolder() / mappings : mappings;
          if (!std::filesystem::exists(path)) {
            REXLOG_WARN(hid, "SDL GameControllerDB: file '{}' does not exist.", path.string());
          } else {
            const int count = SDL_AddGamepadMappingsFromFile(path.string().c_str());
            if (count < 0) {
              REXLOG_ERROR(hid, "SDL GameControllerDB: error loading file '{}': {}.", path.string(),
                           SDL_GetError());
            } else {
              REXLOG_INFO(hid, "SDL GameControllerDB: loaded {} mappings.", count);
            }
          }
        }
        REXLOG_INFO(hid, "SDL input driver initialized");
      });
  if (!ran) {
    state_->running = false;
    state_.reset();
    context.GetSettings().Unregister(*this);
    return ran;
  }
  return {};
}

void SDL3InputDriver::Shutdown() {
  if (!state_) {
    return;
  }
  state_->running = false;
  state_->context->GetSettings().Unregister(*this);
  std::shared_ptr<State> state = std::move(state_);
  const Result<void> ran = state->context->RunOnUIThread([state]() {
    {
      std::lock_guard<std::mutex> guard(state->mutex);
      for (State::Controller& controller : state->controllers) {
        SDL_CloseGamepad(controller.sdl);
      }
      state->controllers.clear();
    }
    if (state->gamepad_up) {
      SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
      state->gamepad_up = false;
    }
    if (state->events_up) {
      SDL_RemoveEventWatch(&State::EventWatch, state.get());
      SDL_QuitSubSystem(SDL_INIT_EVENTS);
      state->events_up = false;
    }
  });
  if (!ran) {
    REXLOG_WARN(hid, "SDL input was not stopped, {}", ran.error().message);
  }
}

u64 SDL3InputDriver::GetDeviceRevision() const {
  if (!state_) {
    return 0;
  }
  state_->QueuePump(state_);
  return state_->revision.load();
}

void SDL3InputDriver::EnumerateDevices(std::vector<PadDevice>& out) {
  out.clear();
  if (!state_ || !state_->gamepad_up) {
    return;
  }
  state_->QueuePump(state_);
  auto guard = state_->DrainAndLock();
  for (const State::Controller& controller : state_->controllers) {
    SDL_Gamepad* sdl = controller.sdl;
    PadDevice device;
    device.id = controller.id;
    const char* name = SDL_GetGamepadName(sdl);
    device.name = name ? name : "";
    device.info.type = ToPadType(SDL_GetGamepadType(sdl));
    device.info.vendor = SDL_GetGamepadVendor(sdl);
    device.info.product = SDL_GetGamepadProduct(sdl);
    device.identity.xinput_slot = controller.xinput_slot;
    device.identity.vendor = device.info.vendor;
    device.identity.product = device.info.product;
    const char* serial = SDL_GetGamepadSerial(sdl);
    device.identity.serial = serial ? serial : "";
    out.push_back(std::move(device));
  }
}

Result<PadState> SDL3InputDriver::GetDeviceState(PadDeviceID id) {
  if (!state_ || !state_->gamepad_up) {
    return std::unexpected(NotConnected());
  }
  state_->QueuePump(state_);
  auto guard = state_->DrainAndLock();
  const State::Controller* controller = state_->FindByID(id);
  if (!controller) {
    return std::unexpected(NotConnected());
  }
  return controller->state;
}

Result<PadCapabilities> SDL3InputDriver::GetDeviceCapabilities(PadDeviceID id) {
  if (!state_ || !state_->gamepad_up) {
    return std::unexpected(NotConnected());
  }
  state_->QueuePump(state_);
  auto guard = state_->DrainAndLock();
  const State::Controller* controller = state_->FindByID(id);
  if (!controller) {
    return std::unexpected(NotConnected());
  }

  SDL_Gamepad* sdl = controller->sdl;
  SDL_Joystick* joystick = SDL_GetGamepadJoystick(sdl);
  u16 flags = 0;
  if (SDL_GetJoystickConnectionState(joystick) == SDL_JOYSTICK_CONNECTION_WIRELESS) {
    flags |= kCapsWireless;
  } else {
    const SDL_PowerState power = SDL_GetJoystickPowerInfo(joystick, nullptr);
    if (power == SDL_POWERSTATE_ON_BATTERY || power == SDL_POWERSTATE_CHARGING ||
        power == SDL_POWERSTATE_CHARGED) {
      flags |= kCapsWireless;
    }
  }
  static constexpr std::array<SDL_GamepadButton, 6> kNavButtons = {
      SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,      SDL_GAMEPAD_BUTTON_DPAD_UP,
      SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
  };
  for (SDL_GamepadButton button : kNavButtons) {
    if (!SDL_GamepadHasButton(sdl, button)) {
      flags |= kCapsNoNavigation;
      break;
    }
  }

  PadCapabilities caps;
  caps.subtype = GamepadSubtype(sdl);
  caps.flags = flags;
  caps.supported.buttons = kSupportedButtons;
  caps.supported.left_trigger = 0xFF;
  caps.supported.right_trigger = 0xFF;
  caps.supported.thumb_lx = static_cast<i16>(0xFFFFu);
  caps.supported.thumb_ly = static_cast<i16>(0xFFFFu);
  caps.supported.thumb_rx = static_cast<i16>(0xFFFFu);
  caps.supported.thumb_ry = static_cast<i16>(0xFFFFu);
  caps.motors.left_motor = 0xFFFF;
  caps.motors.right_motor = 0xFFFF;
  return caps;
}

Result<void> SDL3InputDriver::SetDeviceVibration(PadDeviceID id, const PadVibration& vibration) {
  if (!state_ || !state_->gamepad_up) {
    return std::unexpected(NotConnected());
  }
  state_->QueuePump(state_);
  auto guard = state_->DrainAndLock();
  const State::Controller* controller = state_->FindByID(id);
  if (!controller) {
    return std::unexpected(NotConnected());
  }
  if (!SDL_RumbleGamepad(controller->sdl, vibration.left_motor, vibration.right_motor,
                         kRumbleDurationMs)) {
    return Err(ErrorCategory::kPlatform, "the pad refused the vibration");
  }
  return {};
}

std::string_view SDL3InputProvider::GetName() const {
  return "rexhid-sdl3";
}

Result<void> SDL3InputProvider::Connect(IFactory&) {
  rex::memory::SetTagName(rex::hid::kHID, "HID");
  return {};
}

void SDL3InputProvider::Disconnect() {}

Result<void> SDL3InputProvider::Init() {
  return Ok();
}

void SDL3InputProvider::Shutdown() {}

namespace {

std::unique_ptr<IDriver> CreateSDL3() {
  return std::make_unique<SDL3InputDriver>();
}

constexpr std::array<DriverRegistration, 1> kDrivers = {
    DriverRegistration{DriverType::kInput, "sdl3", {}, &CreateSDL3}};

}  // namespace

std::span<const DriverRegistration> SDL3InputProvider::GetDrivers() const {
  return kDrivers;
}

}  // namespace rex::hid::sdl3

REX_DECLARE_MODULE(rex::hid::sdl3::SDL3InputProvider)
