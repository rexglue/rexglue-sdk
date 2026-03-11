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

namespace rex::filesystem {

NullFile::NullFile(uint32_t file_access, NullEntry* entry) : File(file_access, entry) {}

NullFile::~NullFile() = default;

void NullFile::Destroy() {
  delete this;
}

X_STATUS NullFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset, size_t* out_bytes_read) {
  if (!(file_access_ & FileAccess::kFileReadData)) {
    return X_STATUS_ACCESS_DENIED;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS NullFile::WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                             size_t* out_bytes_written) {
  if (!(file_access_ & (FileAccess::kFileWriteData | FileAccess::kFileAppendData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS NullFile::SetLength(size_t length) {
  if (!(file_access_ & FileAccess::kFileWriteData)) {
    return X_STATUS_ACCESS_DENIED;
  }

  return X_STATUS_SUCCESS;
}

}  // namespace rex::filesystem
