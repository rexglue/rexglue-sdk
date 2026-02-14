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

#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>

namespace rex::filesystem {

class NullDevice;

class NullEntry : public Entry {
 public:
  NullEntry(Device* device, Entry* parent, std::string path);
  ~NullEntry() override;

  static NullEntry* Create(Device* device, Entry* parent, const std::string& path);

  X_STATUS Open(uint32_t desired_access, File** out_file) override;

  bool can_map() const override { return false; }

 private:
  friend class NullDevice;
};

}  // namespace rex::filesystem
