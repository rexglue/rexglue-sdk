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

#pragma once

#include <string>

#include <rex/filesystem/device.h>

namespace rex::filesystem {

class NullEntry;

class NullDevice : public Device {
 public:
  NullDevice(const std::string& mount_path, const std::initializer_list<std::string>& null_paths);
  ~NullDevice() override;

  bool Initialize() override;
  void Dump(string::StringBuffer* string_buffer) override;
  Entry* ResolvePath(const std::string_view path) override;

  bool is_read_only() const override { return false; }

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 40; }

  uint32_t total_allocation_units() const override { return 0x10; }
  uint32_t available_allocation_units() const override { return 0x10; }

  // STFC/cache code seems to require the product of the next two to equal
  // 0x10000
  uint32_t sectors_per_allocation_unit() const override { return 0x80; }

  // STFC requires <= 0x1000
  uint32_t bytes_per_sector() const override { return 0x200; }

 private:
  std::string name_;
  std::unique_ptr<Entry> root_entry_;
  std::vector<std::string> null_paths_;
};

}  // namespace rex::filesystem
