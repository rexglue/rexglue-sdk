/**
 * @file        hid/input_driver.h
 * @brief       Input driver interface
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <vector>

#include <rex/driver.h>
#include <rex/hid/pad.h>
#include <rex/logging/log.h>
#include <rex/memory/host.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::hid {

/** The memory tag of input devices, named "HID". */
inline constexpr memory::Tag kHID{6};

/**
 * A source of pads. It reports devices and their raw state, with the guide button and no deadzone
 * or masking. InputSystem makes one call at a time, from any thread.
 */
class IInputDriver : public IDriver {
 public:
  /** Returns a counter that changes whenever a pad connects or disconnects. */
  virtual u64 GetDeviceRevision() const = 0;

  /**
   * Lists the connected pads.
   *
   * @param out Cleared, then filled.
   */
  virtual void EnumerateDevices(std::vector<PadDevice>& out) = 0;

  /**
   * Reads a pad.
   *
   * @return The raw state, or kNotFound when it disconnected.
   */
  [[nodiscard]] virtual Result<PadState> GetDeviceState(PadDeviceID id) = 0;

  /** Returns what a pad has, guide included, or kNotFound. */
  [[nodiscard]] virtual Result<PadCapabilities> GetDeviceCapabilities(PadDeviceID id) = 0;

  /**
   * Sets a pad's motors.
   *
   * @return Ok, or kNotFound.
   */
  [[nodiscard]] virtual Result<void> SetDeviceVibration(PadDeviceID id,
                                                        const PadVibration& vibration) = 0;
};

}  // namespace rex::hid
