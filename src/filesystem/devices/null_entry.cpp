/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include "null_entry.h"
#include "null_file.h"

#include <rex/filesystem.h>
#include <rex/filesystem/device.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/mapped_memory.h>
#include <rex/string.h>

namespace rex::filesystem {

NullEntry::NullEntry(Device* device, Entry* parent, std::string path)
    : Entry(device, parent, path) {}

NullEntry::~NullEntry() = default;

NullEntry* NullEntry::Create(Device* device, Entry* parent, const std::string& path) {
  auto entry = new NullEntry(device, parent, path);

  entry->create_timestamp_ = 0;
  entry->access_timestamp_ = 0;
  entry->write_timestamp_ = 0;

  entry->attributes_ = kFileAttributeNormal;

  entry->size_ = 0;
  entry->allocation_size_ = 0;
  return entry;
}

X_STATUS NullEntry::Open(uint32_t desired_access, File** out_file) {
  if (is_read_only() &&
      (desired_access & (FileAccess::kFileWriteData | FileAccess::kFileAppendData))) {
    REXFS_ERROR("Attempting to open file for write access on read-only device");
    return X_STATUS_ACCESS_DENIED;
  }

  *out_file = new NullFile(desired_access, this);
  return X_STATUS_SUCCESS;
}

}  // namespace rex::filesystem
