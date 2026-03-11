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

#pragma once

#include <span>

#include <rex/filesystem/file.h>

namespace rex::filesystem {

class DiscImageEntry;

class DiscImageFile : public File {
 public:
  DiscImageFile(uint32_t file_access, DiscImageEntry* entry);
  ~DiscImageFile() override;

  void Destroy() override;

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset, size_t* out_bytes_read) override;
  X_STATUS WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                     size_t* out_bytes_written) override {
    (void)buffer;
    (void)byte_offset;
    (void)out_bytes_written;
    return X_STATUS_ACCESS_DENIED;
  }
  X_STATUS SetLength(size_t length) override {
    (void)length;
    return X_STATUS_ACCESS_DENIED;
  }

 private:
  DiscImageEntry* entry_;
};

}  // namespace rex::filesystem
