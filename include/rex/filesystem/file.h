/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>

#include <rex/system/xtypes.h>

namespace rex::filesystem {

class Entry;

class File {
 public:
  File(uint32_t file_access, Entry* entry) : file_access_(file_access), entry_(entry) {}
  virtual ~File() = default;

  virtual void Destroy() = 0;

  virtual X_STATUS ReadSync(void* buffer, size_t buffer_length, size_t byte_offset,
                            size_t* out_bytes_read) = 0;
  virtual X_STATUS WriteSync(const void* buffer, size_t buffer_length, size_t byte_offset,
                             size_t* out_bytes_written) = 0;

  // TODO: Parameters
  virtual X_STATUS ReadAsync(void* buffer, size_t buffer_length, size_t byte_offset,
                             size_t* out_bytes_read) {
    (void)buffer;
    (void)buffer_length;
    (void)byte_offset;
    (void)out_bytes_read;

    return X_STATUS_NOT_IMPLEMENTED;
  }

  // TODO: Parameters
  virtual X_STATUS WriteAsync(const void* buffer, size_t buffer_length, size_t byte_offset,
                              size_t* out_bytes_written) {
    (void)buffer;
    (void)buffer_length;
    (void)byte_offset;
    (void)out_bytes_written;

    return X_STATUS_NOT_IMPLEMENTED;
  }

  virtual X_STATUS SetLength(size_t length) {
    (void)length;
    return X_STATUS_NOT_IMPLEMENTED;
  }

  // rex::filesystem::FileAccess
  uint32_t file_access() const { return file_access_; }
  const Entry* entry() const { return entry_; }
  Entry* entry() { return entry_; }

 protected:
  // rex::filesystem::FileAccess
  uint32_t file_access_ = 0;
  Entry* entry_ = nullptr;
};

}  // namespace rex::filesystem
