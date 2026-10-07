/**
 * @file        hid/driver.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <vector>

#include <rex/hid/device.h>
#include <rex/hid/error.h>
#include <rex/hid/event.h>
#include <rex/hid/input_host.h>
#include <rex/hid/xinput.h>
#include <rex/result.h>

namespace rex::hid {

/** A source of devices. The backend assigns its devices to title users. */
class Driver {
 public:
  virtual ~Driver() = default;

  /**
   * Lists the open devices in arrival order. DeviceInfo::ordinal stays 0, the backend assigns it.
   *
   * @param out Receives the devices.
   */
  virtual void EnumerateDevices(std::vector<DeviceInfo>& out) = 0;

  /**
   * Reads a device.
   *
   * @param id A device of this driver.
   * @return The state, or a HID error.
   */
  [[nodiscard]] virtual Result<X_INPUT_STATE> GetDeviceState(DeviceId id) = 0;

  /**
   * Reads the capabilities of a device.
   *
   * @param id A device of this driver.
   * @param flags The XINPUT_FLAG bits the title passed.
   * @return The capabilities, or a HID error.
   */
  [[nodiscard]] virtual Result<X_INPUT_CAPABILITIES> GetDeviceCapabilities(DeviceId id,
                                                                           u32 flags) = 0;

  /**
   * Sets the motor speeds of a device.
   *
   * @param id A device of this driver.
   * @param vibration The speeds.
   * @return Ok, or a HID error.
   */
  [[nodiscard]] virtual Result<void> SetDeviceVibration(DeviceId id,
                                                        const X_INPUT_VIBRATION& vibration) = 0;

  /**
   * Takes the next keystroke of a device.
   *
   * @param id A device of this driver.
   * @param flags The XINPUT_FLAG bits the title passed.
   * @return The keystroke, or the HID error Empty when none is queued.
   */
  [[nodiscard]] virtual Result<X_INPUT_KEYSTROKE> GetDeviceKeystroke(DeviceId id, u32 flags) = 0;

  /**
   * Receives a window input event on the UI thread.
   *
   * @param event The event.
   */
  virtual void OnEvent(const InputEvent& event) { (void)event; }

  /**
   * Attaches the input host, null to detach. Called on the UI thread.
   *
   * @param host The host.
   */
  void AttachHost(IInputHost* host) {
    host_.store(host, std::memory_order_release);
    OnHostAttached(host);
  }

 protected:
  /**
   * Runs UI thread setup or teardown when the host changes.
   *
   * @param host The new host, null on detach.
   */
  virtual void OnHostAttached(IInputHost* host) { (void)host; }

  /**
   * Returns the input host.
   *
   * @return The host, null while detached.
   */
  IInputHost* host() const { return host_.load(std::memory_order_acquire); }

  /**
   * Reports whether input reaches the title.
   *
   * @return True without a host, otherwise IInputHost::capture_active.
   */
  bool is_active() const {
    IInputHost* current = host();
    return !current || current->capture_active();
  }

 private:
  std::atomic<IInputHost*> host_{nullptr};
};

}  // namespace rex::hid
