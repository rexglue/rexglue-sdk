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

#include <rex/filesystem/devices/stfs_container_entry.h>
#include <rex/filesystem/devices/stfs_container_file.h>

#include <map>

#include <rex/math.h>

namespace rex::filesystem {

StfsContainerEntry::StfsContainerEntry(Device* device, Entry* parent, const std::string_view path,
                                       MultiFileHandles* files)
    : Entry(device, parent, path), files_(files), data_offset_(0), data_size_(0), block_(0) {}

StfsContainerEntry::~StfsContainerEntry() = default;

std::unique_ptr<StfsContainerEntry> StfsContainerEntry::Create(Device* device, Entry* parent,
                                                               const std::string_view name,
                                                               MultiFileHandles* files) {
  auto path = rex::string::utf8_join_guest_paths(parent->path(), name);
  auto entry = std::make_unique<StfsContainerEntry>(device, parent, path, files);

  return std::move(entry);
}

X_STATUS StfsContainerEntry::Open(uint32_t desired_access, File** out_file) {
  *out_file = new StfsContainerFile(desired_access, this);
  return X_STATUS_SUCCESS;
}

}  // namespace rex::filesystem
