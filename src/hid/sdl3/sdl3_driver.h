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

#pragma once

#include <filesystem>

#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

#include <rex/hid/driver.h>

#include <SDL3/SDL.h>

inline constexpr i32 HID_SDL_THUMB_THRES = 0x4E00;
inline constexpr i32 HID_SDL_TRIGG_THRES = 0x1F;
inline constexpr i32 HID_SDL_REPEAT_DELAY = 400;
inline constexpr i32 HID_SDL_REPEAT_RATE = 100;

namespace rex::hid::sdl3 {

class SDL3Driver final : public Driver {
 public:
  explicit SDL3Driver(std::filesystem::path mappings_file);
  ~SDL3Driver() override;

  [[nodiscard]] Result<void> Setup();

  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  [[nodiscard]] Result<X_INPUT_STATE> GetDeviceState(DeviceId id) override;
  [[nodiscard]] Result<X_INPUT_CAPABILITIES> GetDeviceCapabilities(DeviceId id, u32 flags) override;
  [[nodiscard]] Result<void> SetDeviceVibration(DeviceId id,
                                                const X_INPUT_VIBRATION& vibration) override;
  [[nodiscard]] Result<X_INPUT_KEYSTROKE> GetDeviceKeystroke(DeviceId id, u32 flags) override;

 protected:
  void OnHostAttached(IInputHost* host) override;

 private:
  enum class RepeatState {
    Idle,       // no buttons pressed or repeating has ended
    Waiting,    // a button is held and the delay is awaited
    Repeating,  // actively repeating at a rate
  };
  struct KeystrokeState {
    uint64_t buttons;
    RepeatState repeat_state;
    // the button number that was pressed last:
    uint8_t repeat_butt_idx;
    // the last time (ms) a down (and/or repeat) event for that button was send:
    uint32_t repeat_time;
  };

  struct ControllerState {
    SDL_Gamepad* sdl;
    X_INPUT_CAPABILITIES caps;
    X_INPUT_STATE state;
    bool state_changed;
    bool is_active;
    DeviceId id;
    // Per pad rather than per guest user, so it survives reassignment.
    KeystrokeState keystroke;
  };

  X_RESULT ReadDeviceState(DeviceId id, X_INPUT_STATE* out_state);
  X_RESULT ReadDeviceCapabilities(DeviceId id, uint32_t flags, X_INPUT_CAPABILITIES* out_caps);
  X_RESULT WriteDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration);
  X_RESULT ReadDeviceKeystroke(DeviceId id, uint32_t flags, X_INPUT_KEYSTROKE* out_keystroke);

  static bool EventWatch(void* userdata, SDL_Event* event);
  void HandleEvent(const SDL_Event& event);
  std::unique_lock<std::mutex> DrainAndLock();
  void ProcessEventLocked(const SDL_Event& event);
  void OnJoystickDeviceAddedLocked(const SDL_Event& event);
  void OnControllerDeviceAddedLocked(const SDL_Event& event);
  void OnControllerDeviceRemovedLocked(const SDL_Event& event);
  void OnControllerDeviceAxisMotionLocked(const SDL_Event& event);
  void OnControllerDeviceButtonChangedLocked(const SDL_Event& event);

  inline uint64_t AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const;
  std::optional<size_t> GetControllerIndexFromInstanceID(SDL_JoystickID instance_id);
  ControllerState* FindController(DeviceId id);
  DeviceId AllocateDeviceId();
  bool TestSDLVersion() const;
  void UpdateXCapabilities(ControllerState& state);
  void QueueControllerUpdate();

  bool attached_ = false;
  bool sdl_events_initialized_;
  bool SDL_Gamepad_initialized_;
  std::filesystem::path mappings_file_;
  std::atomic<int> sdl_events_unflushed_;
  std::atomic<bool> sdl_pumpevents_queued_;
  // Appended in connection order, never re-sorted, and unbounded: the
  // assignment decides how many the guest sees.
  std::vector<ControllerState> controllers_;
  std::mutex controllers_mutex_;
  std::mutex event_queue_mutex_;
  std::vector<SDL_Event> pending_events_;
  // Never rewound, so a handle held past a disconnect resolves to nothing.
  uint64_t next_device_id_ = 1;
};

}  // namespace rex::hid::sdl3
