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

#include <rex/filesystem/device.h>
#include <rex/logging.h>

namespace rex::filesystem {

Device::Device(const std::string_view mount_path) : mount_path_(mount_path) {}
Device::~Device() = default;

}  // namespace rex::filesystem
