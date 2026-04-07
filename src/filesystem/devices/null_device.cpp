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

#include <rex/filesystem/devices/null_entry.h>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/null_device.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/string.h>

namespace rex::filesystem {

NullDevice::NullDevice(const std::string& mount_path,
                       const std::initializer_list<std::string>& null_paths)
    : Device(mount_path), null_paths_(null_paths), name_("NullDevice") {}

NullDevice::~NullDevice() = default;

bool NullDevice::Initialize() {
  auto root_entry = new NullEntry(this, nullptr, mount_path_);
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);

  for (auto path : null_paths_) {
    auto child = NullEntry::Create(this, root_entry, path);
    root_entry->children_.push_back(std::unique_ptr<Entry>(child));
  }
  return true;
}

void NullDevice::Dump(string::StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* NullDevice::ResolvePath(const std::string_view path) {
  REXFS_INFO("NullDevice::ResolvePath({})", path);

  auto root = root_entry_.get();
  if (path.empty()) {
    return root_entry_.get();
  }

  for (auto& child : root->children()) {
    if (!rex::string::compare_case(child->path().c_str(), path.data())) {
      return child.get();
    }
  }

  return nullptr;
}

}  // namespace rex::filesystem
