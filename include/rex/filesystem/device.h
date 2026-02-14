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

#include <memory>
#include <string>

#include <rex/filesystem/entry.h>
#include <rex/string/buffer.h>
#include <rex/thread/mutex.h>

namespace rex::filesystem {

class Device {
 public:
  explicit Device(const std::string_view mount_path);
  virtual ~Device();

  virtual bool Initialize() = 0;

  const std::string& mount_path() const { return mount_path_; }
  virtual bool is_read_only() const { return true; }

  virtual void Dump(string::StringBuffer* string_buffer) = 0;
  virtual Entry* ResolvePath(const std::string_view path) = 0;

  virtual const std::string& name() const = 0;
  virtual uint32_t attributes() const = 0;
  virtual uint32_t component_name_max_length() const = 0;

  virtual uint32_t total_allocation_units() const = 0;
  virtual uint32_t available_allocation_units() const = 0;
  virtual uint32_t sectors_per_allocation_unit() const = 0;
  virtual uint32_t bytes_per_sector() const = 0;

 protected:
  rex::thread::global_critical_region global_critical_region_;
  std::string mount_path_;
};

}  // namespace rex::filesystem
