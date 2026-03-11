/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include "disc_image_entry.h"
#include "disc_image_file.h"

#include <algorithm>

namespace rex::filesystem {

DiscImageFile::DiscImageFile(uint32_t file_access, DiscImageEntry* entry)
    : File(file_access, entry), entry_(entry) {}

DiscImageFile::~DiscImageFile() = default;

void DiscImageFile::Destroy() {
  delete this;
}

X_STATUS DiscImageFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                 size_t* out_bytes_read) {
  if (byte_offset >= entry_->size()) {
    return X_STATUS_END_OF_FILE;
  }
  size_t real_offset = entry_->data_offset() + byte_offset;
  size_t real_length = std::min(buffer.size(), entry_->data_size() - byte_offset);
  std::memcpy(buffer.data(), entry_->mmap()->data() + real_offset, real_length);
  *out_bytes_read = real_length;
  return X_STATUS_SUCCESS;
}

}  // namespace rex::filesystem
