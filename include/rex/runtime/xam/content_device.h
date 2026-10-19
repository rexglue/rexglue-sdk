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

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/export.h>
#include <rex/runtime/xtypes.h>

namespace rex {
namespace runtime {

/** Storage device type. */
enum class DeviceType : u8 {
  kHDD = 1, /**< Hard disk. */
  kODD = 4, /**< Optical disc. */
};

/** Ids of the placeholder devices reported to the title. */
enum class DummyDeviceId : u8 {
  kHDD = 1, /**< Placeholder hard disk. */
  kODD = 2, /**< Placeholder optical disc. */
};

/** Description of a placeholder storage device. */
struct PlaceholderDevice {
  DummyDeviceId device_id;        /**< Id the title refers to the device by. */
  DeviceType device_type;         /**< Device type. */
  u64 total_bytes;                /**< Capacity in bytes. */
  u64 free_bytes;                 /**< Free space in bytes. */
  const std::u16string_view name; /**< Display name. */
};

/**
 * Looks up a placeholder device.
 *
 * @param device_id The DummyDeviceId value.
 * @return The device, null when no placeholder has the id.
 */
REX_RUNTIME_API const PlaceholderDevice* GetPlaceholderDevice(u32 device_id);

}  // namespace runtime
}  // namespace rex
