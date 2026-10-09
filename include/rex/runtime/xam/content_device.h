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

#include <rex/macros.h>
#include <rex/runtime/xtypes.h>

namespace rex {
namespace runtime {
namespace xam {

/** Storage device type. */
enum class DeviceType : uint32_t {
  HDD = 1, /**< Hard disk. */
  ODD = 4, /**< Optical disc. */
};

/** Ids of the placeholder devices reported to the title. */
enum class DummyDeviceId : uint32_t {
  HDD = 1, /**< Placeholder hard disk. */
  ODD = 2, /**< Placeholder optical disc. */
};

/** Description of a placeholder storage device. */
struct DummyDeviceInfo {
  DummyDeviceId device_id;        /**< Id the title refers to the device by. */
  DeviceType device_type;         /**< Device type. */
  uint64_t total_bytes;           /**< Capacity in bytes. */
  uint64_t free_bytes;            /**< Free space in bytes. */
  const std::u16string_view name; /**< Display name. */
};

/**
 * Looks up a placeholder device.
 *
 * @param device_id The DummyDeviceId value.
 * @return The device, null when no placeholder has the id.
 */
REX_RUNTIME_API const DummyDeviceInfo* GetDummyDeviceInfo(uint32_t device_id);

}  // namespace xam
}  // namespace runtime
}  // namespace rex
