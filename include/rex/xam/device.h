/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        xam/device.h
 * @brief       Storage device types and the placeholder devices reported to the title
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * @remarks     Parts derived from Xenia (Ben Vanik, 2020).
 * @remarks     Parts derived from Xenia (Ben Vanik, 2022).
 */

#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>
#include <format>
#include <string>
#include <filesystem>
#include <optional>
#include <string_view>
#include <functional>
#include <span>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/memory.h>
#include <rex/kernel/types/status.h>
#include <rex/memory/stream.h>
#include <rex/kernel/types/content.h>
#include <rex/result.h>
#include <rex/filesystem/xdbf.h>
#include <rex/utility.h>
#include <rex/title.h>

namespace rex::xam {

enum class DeviceType : u8 {
  kHDD = 1,
  kODD = 4,
};

enum class DummyDeviceID : u8 {
  kHDD = 1,
  kODD = 2,
};

struct PlaceholderDevice {
  DummyDeviceID device_id;
  DeviceType device_type;
  u64 total_bytes;
  u64 free_bytes;
  const std::u16string_view name;
};

/**
 * Looks up a placeholder device.
 *
 * @param device_id The DummyDeviceID value.
 * @return The device, null when no placeholder has the ID.
 */
REX_XAM_API const PlaceholderDevice* GetPlaceholderDevice(u32 device_id);

}  // namespace rex::xam
