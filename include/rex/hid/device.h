/**
 * @file        hid/device.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>

#include <rex/types.h>

namespace rex::hid {

/** The number of title users. */
constexpr u32 kMaxTitleUsers = 4;

/**
 * Driver scoped device handle. Never reused within a process run, so a handle that outlives its
 * device resolves to nothing rather than aliasing whichever device took the freed slot.
 */
// NOLINTNEXTLINE(performance-enum-size): u64 counter, ids never reused
enum class DeviceId : u64 { kInvalid = 0 };

/** A device a driver reports. */
struct DeviceInfo {
  /** The driver scoped handle. */
  DeviceId id = DeviceId::kInvalid;
  /** Connection order, assigned by the backend. */
  u32 ordinal = 0;
  /** The device name. */
  std::string name;
  /** The device GUID. */
  std::string guid;
  /** XINPUT_DEVSUBTYPE_*. */
  u8 subtype = 0x01;
  /** Keyboard/mouse emulation or the null backend pad. */
  bool synthetic = false;
};

}  // namespace rex::hid
