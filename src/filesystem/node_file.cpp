/**
 * @file        filesystem/node_file.cpp
 * @brief       IFile and IDirectory over a shared tree node
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/file.h and xenia/kernel/xfile.cc (Ben Vanik, 2015).
 */

#include "filesystem/node_file.h"

#include <format>

#include <rex/filesystem/error.h>

#include "filesystem/provider.h"

namespace rex::filesystem {

NodeFile::NodeFile(Node& node, Access access, std::unique_ptr<NodeStream> stream)
    : node_(&node), access_(access), stream_(std::move(stream)) {}

FileInfo NodeFile::Info() const {
  return node_->Info();
}

void NodeFile::Refresh() {
  node_->Refresh();
}

VolumeInfo NodeFile::Volume() const {
  return node_->provider().Volume();
}

Result<size_t> NodeFile::Read(std::span<u8> buffer, u64 offset) {
  return stream_->Read(buffer, offset);
}

Result<size_t> NodeFile::Write(std::span<const u8> buffer, u64 offset) {
  return stream_->Write(buffer, offset);
}

Result<void> NodeFile::SetLength(u64 length) {
  return stream_->SetLength(length);
}

Result<void> NodeFile::SetAttributes(FileAttributes attributes) {
  return node_->SetAttributes(attributes);
}

Result<void> NodeFile::SetTimes(const FileTimes& times) {
  return node_->SetTimes(times);
}

void NodeFile::SetDeleteOnClose(bool delete_on_close) {
  node_->set_delete_on_close(delete_on_close);
}

Result<void> NodeFile::Rename(std::string_view path) {
  return node_->Rename(path);
}

Result<std::unique_ptr<IDirectory>> NodeFile::List(std::string_view pattern) {
  if (!node_->is_directory()) {
    return Err<std::unique_ptr<IDirectory>>(MakeError(
        Error::kNotDirectory, std::format("'{}' is not a directory", node_->absolute_path())));
  }
  return std::unique_ptr<IDirectory>(std::make_unique<NodeDirectory>(*node_, pattern));
}

Result<std::span<const u8>> NodeFile::View() {
  return stream_->View();
}

NodeDirectory::NodeDirectory(Node& node, std::string_view pattern)
    : node_(&node), matcher_(pattern) {}

std::optional<FileInfo> NodeDirectory::Next() {
  Node* child = node_->IterateChildren(matcher_, &index_);
  if (!child) {
    return std::nullopt;
  }
  return child->Info();
}

}  // namespace rex::filesystem
