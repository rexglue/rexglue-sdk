#pragma once
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

#include <rex/system/xtypes.h>

namespace rex {
namespace system {
namespace xam {

enum class DeviceType : uint32_t {
  HDD = 1,
  ODD = 4,
};

enum class DummyDeviceId : uint32_t {
  HDD = 1,
  ODD = 2,
};

struct DummyDeviceInfo {
  DummyDeviceId device_id;
  DeviceType device_type;
  uint64_t total_bytes;
  uint64_t free_bytes;
  const std::u16string_view name;
};

const DummyDeviceInfo* GetDummyDeviceInfo(uint32_t device_id);

}  // namespace xam
}  // namespace system
}  // namespace rex
