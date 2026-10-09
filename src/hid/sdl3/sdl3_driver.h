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
    Idle,
    Waiting,
    Repeating,
  };
  struct KeystrokeState {
    u64 buttons;
    RepeatState repeat_state;
    u8 repeat_butt_idx;
    /** Time in ms of the last down or repeat event for that button. */
    u32 repeat_time;
  };

  struct ControllerState {
    SDL_Gamepad* sdl;
    X_INPUT_CAPABILITIES caps;
    X_INPUT_STATE state;
    bool state_changed;
    bool is_active;
    DeviceId id;
    /** Kept per pad, so it survives title user reassignment. */
    KeystrokeState keystroke;
  };

  X_RESULT ReadDeviceState(DeviceId id, X_INPUT_STATE* out_state);
  X_RESULT ReadDeviceCapabilities(DeviceId id, X_INPUT_CAPABILITIES* out_caps);
  /**
   * Starts rumble on a pad. XInput vibration holds until the guest changes it but SDL rumble
   * expires, and a zero duration expires on the next SDL_UpdateJoysticks, so the maximum duration
   * is armed. Each call cancels the previous effect.
   *
   * @param id         Device handle.
   * @param vibration  Motor speeds.
   * @return X_ERROR_SUCCESS, X_ERROR_DEVICE_NOT_CONNECTED or X_ERROR_FUNCTION_FAILED.
   */
  X_RESULT WriteDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration);
  /**
   * Synthesizes keystrokes from pad button changes. The flags argument of the guest call is not
   * used. The keystroke table is indexed by bit and its order is the order events are sent when
   * several buttons change at once. Bits 0 to 15 are the XInput button field, bits 16 and 17 the
   * triggers as buttons, bits 18 to 25 the left stick directions and bits 26 to 33 the right
   * stick directions.
   *
   * @param id             Device handle.
   * @param out_keystroke  Receives the keystroke.
   * @return X_ERROR_SUCCESS when a keystroke was produced, X_ERROR_EMPTY when none,
   *         X_ERROR_DEVICE_NOT_CONNECTED or X_ERROR_BAD_ARGUMENTS on failure.
   */
  X_RESULT ReadDeviceKeystroke(DeviceId id, X_INPUT_KEYSTROKE* out_keystroke);

  static bool EventWatch(void* userdata, SDL_Event* event);
  /**
   * Buffers an SDL event. Runs on the thread that posts the event, which can be a thread SDL
   * created for the joystick subsystem. It only buffers and never takes controllers_mutex_, which
   * avoids a lock ordering inversion with the SDL joystick lock.
   *
   * @param event  The SDL event.
   */
  void HandleEvent(const SDL_Event& event);
  std::unique_lock<std::mutex> DrainAndLock();
  void ProcessEventLocked(const SDL_Event& event);
  void OnJoystickDeviceAddedLocked(const SDL_Event& event);
  void OnControllerDeviceAddedLocked(const SDL_Event& event);
  void OnControllerDeviceRemovedLocked(const SDL_Event& event);
  void OnControllerDeviceAxisMotionLocked(const SDL_Event& event);
  /**
   * Applies a pad button change. The lookup table is indexed by SDL_GamepadButton. Entries 15 to
   * 20 cover Misc1, the 4 paddles and the touchpad, mapped to GUIDE and the face buttons.
   *
   * @param event  The SDL button event.
   */
  void OnControllerDeviceButtonChangedLocked(const SDL_Event& event);

  /**
   * Builds the keystroke bit field for analog inputs past their thresholds.
   *
   * @param gamepad  Gamepad report.
   * @return Bit field with the trigger and stick direction bits set.
   */
  inline u64 AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const;
  std::optional<size_t> GetControllerIndexFromInstanceID(SDL_JoystickID instance_id);
  ControllerState* FindController(DeviceId id);
  DeviceId AllocateDeviceId();
  bool TestSDLVersion() const;
  /**
   * Refreshes the cached capabilities. The RAWINPUT driver correlates input from several APIs
   * with latency, so the power level is only available once the pad is touched, which can confuse
   * titles calling GetCapabilities. Disabling the RAWINPUT driver with its SDL hint avoids it.
   *
   * @param state  Controller to refresh.
   */
  void UpdateXCapabilities(ControllerState& state);
  void QueueControllerUpdate();

  bool attached_ = false;
  bool sdl_events_initialized_;
  bool SDL_Gamepad_initialized_;
  std::filesystem::path mappings_file_;
  std::atomic<int> sdl_events_unflushed_;
  std::atomic<bool> sdl_pumpevents_queued_;
  /**
   * Appended in connection order, never re-sorted and unbounded. The assignment decides how
   * many the title sees.
   */
  std::vector<ControllerState> controllers_;
  std::mutex controllers_mutex_;
  std::mutex event_queue_mutex_;
  std::vector<SDL_Event> pending_events_;
  /** Never rewound, so a handle held past a disconnect resolves to nothing. */
  u64 next_device_id_ = 1;
};

}  // namespace rex::hid::sdl3
