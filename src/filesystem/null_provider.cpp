/**
 * @file        filesystem/null_provider.cpp
 * @brief       The provider that answers a fixed list of paths with empty files
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/devices/null_device.cc, null_entry.cc and
 *              null_file.cc (Ben Vanik, 2020).
 */

#include "filesystem/null_provider.h"

#include <format>

#include <rex/filesystem/error.h>
#include <rex/logging.h>
#include <rex/string.h>

namespace rex::filesystem {

namespace {

class NullStream final : public NodeStream {
 public:
  explicit NullStream(Access access) : access_(access) {}

  [[nodiscard]] Result<size_t> Read(std::span<u8> buffer, u64 offset) override {
    (void)buffer;
    (void)offset;
    if (!HasFlags(access_, Access::kReadData)) {
      return Err<size_t>(MakeError(Error::kAccessDenied, "the file was opened without read"));
    }
    return size_t{0};
  }

  [[nodiscard]] Result<size_t> Write(std::span<const u8> buffer, u64 offset) override {
    (void)buffer;
    (void)offset;
    if ((access_ & (Access::kWriteData | Access::kAppendData)) == Access::kNone) {
      return Err<size_t>(MakeError(Error::kAccessDenied, "the file was opened without write"));
    }
    return size_t{0};
  }

  [[nodiscard]] Result<void> SetLength(u64 length) override {
    (void)length;
    if (!HasFlags(access_, Access::kWriteData)) {
      return Err(MakeError(Error::kAccessDenied, "the file was opened without write"));
    }
    return Ok();
  }

 private:
  Access access_;
};

}  // namespace

NullProvider::NullProvider(Key, MountEntry entry) : Provider(std::move(entry)) {}

Result<std::unique_ptr<Provider>> NullProvider::Create(const MountEntry& entry) {
  auto provider = std::make_unique<NullProvider>(Key{}, entry);
  auto root = std::make_unique<NullNode>(*provider, nullptr, std::string());
  for (const std::string& path : entry.null_paths) {
    root->AddChild(std::make_unique<NullNode>(*provider, root.get(), path));
  }
  provider->root_ = std::move(root);
  return std::unique_ptr<Provider>(std::move(provider));
}

VolumeInfo NullProvider::Volume() const {
  return {"NullDevice", 0, 40, 0x10, 0x10, 0x80, 0x200};
}

Node* NullProvider::Resolve(std::string_view path) {
  REXLOG_DEBUG(filesystem, "null provider resolves '{}'", path);
  if (path.empty()) {
    return root_.get();
  }
  for (const auto& child : root_->children()) {
    if (utf8_equal_case(child->path(), path)) {
      return child.get();
    }
  }
  return nullptr;
}

NullNode::NullNode(NullProvider& provider, Node* parent, std::string path)
    : Node(provider, parent, std::move(path)) {
  attributes_ = parent ? FileAttributes::kNormal : FileAttributes::kDirectory;
}

Result<std::unique_ptr<NodeStream>> NullNode::Open(Access access) {
  if (read_only() && (access & (Access::kWriteData | Access::kAppendData)) != Access::kNone) {
    return Err<std::unique_ptr<NodeStream>>(MakeError(
        Error::kAccessDenied, std::format("'{}' is on a read only mount", absolute_path_)));
  }
  return std::unique_ptr<NodeStream>(std::make_unique<NullStream>(access));
}

}  // namespace rex::filesystem
