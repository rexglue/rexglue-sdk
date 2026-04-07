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

#include <string>
#include <vector>

#include <rex/filesystem/entry.h>
#include <rex/memory/mapped_memory.h>

namespace rex::filesystem {

class DiscImageDevice;

class DiscImageEntry : public Entry {
 public:
  DiscImageEntry(Device* device, Entry* parent, const std::string_view path,
                 memory::MappedMemory* mmap);
  ~DiscImageEntry() override;

  static std::unique_ptr<DiscImageEntry> Create(Device* device, Entry* parent,
                                                const std::string_view name,
                                                memory::MappedMemory* mmap);

  memory::MappedMemory* mmap() const { return mmap_; }
  size_t data_offset() const { return data_offset_; }
  size_t data_size() const { return data_size_; }

  X_STATUS Open(uint32_t desired_access, File** out_file) override;

  bool can_map() const override { return true; }
  std::unique_ptr<memory::MappedMemory> OpenMapped(memory::MappedMemory::Mode mode, size_t offset,
                                                   size_t length) override;

 private:
  friend class DiscImageDevice;

  memory::MappedMemory* mmap_;
  size_t data_offset_;
  size_t data_size_;
};

}  // namespace rex::filesystem
