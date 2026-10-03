/**
 * @file        filesystem/host_provider.cpp
 * @brief       The provider that serves a host folder
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/devices/host_path_device.cc, host_path_entry.cc
 *              and host_path_file.cc (Ben Vanik, 2020).
 */

#include "filesystem/host_provider.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <mutex>
#include <system_error>

#include <rex/filesystem/error.h>
#include <rex/logging.h>
#include <rex/macros.h>
#include <rex/math.h>
#include <rex/memory/mapped_memory.h>
#include <rex/string.h>

namespace rex::filesystem {

namespace {

constexpr u32 kBytesPerSector = 0x200;
constexpr u32 kAllocationUnits = 128 * 1024;

bool IsHostName(std::string_view name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find_first_of(":<>\"|?*\\/") == std::string_view::npos &&
         std::ranges::none_of(name, [](char c) { return static_cast<u8>(c) < 0x20; });
}

class HostStream final : public NodeStream {
 public:
  HostStream(HostNode& node, Access access, std::unique_ptr<HostFile> file)
      : node_(node), access_(access), file_(std::move(file)) {}

  Result<size_t> Read(std::span<u8> buffer, u64 offset) override {
    if (!CanRead(access_)) {
      return Err<size_t>(MakeError(Error::kAccessDenied, "the file was opened without read"));
    }
    REX_TRY(const size_t count, file_->Read(offset, buffer));
    if (count == 0 && !buffer.empty()) {
      return Err<size_t>(
          MakeError(Error::kEndOfFile, "read starts at or past the end of the file"));
    }
    return count;
  }

  Result<size_t> Write(std::span<const u8> buffer, u64 offset) override {
    if (!CanWrite(access_)) {
      return Err<size_t>(MakeError(Error::kAccessDenied, "the file was opened without write"));
    }
    return file_->Write(offset, buffer);
  }

  Result<void> SetLength(u64 length) override {
    if (!CanSetLength(access_)) {
      return Err(MakeError(Error::kAccessDenied, "the file was opened without write"));
    }
    return file_->SetLength(length);
  }

  Result<void> Flush() override { return file_->Flush(); }

  Result<std::span<const u8>> View() override {
    if (!map_) {
      const std::filesystem::path host_path = node_.host_path();
      map_ = memory::MappedMemory::Open(host_path, memory::MappedMemory::Mode::kRead);
      if (!map_) {
        return Err<std::span<const u8>>(
            MakeError(Error::kIO, std::format("cannot map '{}'", path_to_utf8(host_path))));
      }
    }
    return std::span<const u8>(map_->data(), map_->size());
  }

 private:
  HostNode& node_;
  Access access_;
  std::unique_ptr<HostFile> file_;
  std::unique_ptr<memory::MappedMemory> map_;
};

}  // namespace

HostProvider::HostProvider(MountEntry entry) : Provider(std::move(entry)) {}

Result<std::unique_ptr<Provider>> HostProvider::Create(const MountEntry& entry) {
  std::unique_ptr<HostProvider> provider(new HostProvider(entry));
  std::error_code ec;
  if (!std::filesystem::exists(entry.source, ec)) {
    if (provider->read_only()) {
      return Err<std::unique_ptr<Provider>>(
          MakeError(Error::kNotFound,
                    std::format("host folder '{}' does not exist", path_to_utf8(entry.source))));
    }
    std::filesystem::create_directories(entry.source, ec);
  }
  auto root = std::make_unique<HostNode>(*provider, nullptr, std::string(), entry.source);
  root->Populate();
  provider->root_ = std::move(root);
  return std::unique_ptr<Provider>(std::move(provider));
}

VolumeInfo HostProvider::Volume() const {
  return {"STFS", 0, 255, kAllocationUnits, kAllocationUnits, 1, kBytesPerSector};
}

Node* HostProvider::Resolve(std::string_view path) {
  if (Node* resolved = root_->ResolvePath(path)) {
    return resolved;
  }
  std::lock_guard lock(mutex());
  auto* current = static_cast<HostNode*>(root_.get());
  for (std::string_view part : utf8_split_path(path)) {
    if (part.empty()) {
      continue;
    }
    if (Node* child = current->GetChild(part)) {
      current = static_cast<HostNode*>(child);
      continue;
    }
    if (!IsHostName(part)) {
      return nullptr;
    }
    std::optional<HostInfo> info = HostStat(current->host_path() / to_path(part));
    if (!info) {
      std::vector<HostInfo> children = HostList(current->host_path());
      const auto match = std::ranges::find_if(children, [&](const HostInfo& candidate) {
        return utf8_equal_case(path_to_utf8(candidate.name), part);
      });
      if (match == children.end()) {
        return nullptr;
      }
      info = *match;
    }
    current = static_cast<HostNode*>(current->AddChild(HostNode::Create(*this, *current, *info)));
  }
  return current;
}

HostNode::HostNode(HostProvider& provider, Node* parent, std::string path,
                   std::filesystem::path host_path)
    : Node(provider, parent, std::move(path)), host_path_(std::move(host_path)) {
  if (!parent) {
    attributes_ = FileAttributes::kDirectory;
  }
}

std::filesystem::path HostNode::host_path() const {
  std::lock_guard lock(provider().mutex());
  return host_path_;
}

std::unique_ptr<HostNode> HostNode::Create(HostProvider& provider, HostNode& parent,
                                           const HostInfo& info) {
  auto node = std::make_unique<HostNode>(
      provider, &parent, utf8_join_guest_paths(parent.path(), path_to_utf8(info.name)),
      parent.host_path() / info.name);
  node->create_time_ = info.create_time;
  node->access_time_ = info.access_time;
  node->write_time_ = info.write_time;
  if (info.directory) {
    node->attributes_ = FileAttributes::kDirectory;
  } else {
    node->attributes_ = FileAttributes::kNormal;
    if (provider.read_only()) {
      node->attributes_ |= FileAttributes::kReadOnly;
    }
    node->size_ = info.size;
    node->allocation_size_ = round_up(info.size, u64{kBytesPerSector});
  }
  return node;
}

void HostNode::Populate() {
  std::vector<std::pair<u64, u64>> ancestors;
  if (const std::optional<HostInfo> self = HostStat(host_path_)) {
    ancestors.emplace_back(self->device, self->inode);
  }
  Populate(ancestors);
}

void HostNode::Populate(std::vector<std::pair<u64, u64>>& ancestors) {
  for (const HostInfo& info : HostList(host_path_)) {
    auto* child = static_cast<HostNode*>(AddChild(Create(host(), *this, info)));
    if (!info.directory) {
      continue;
    }
    const std::pair<u64, u64> id{info.device, info.inode};
    if (info.inode && std::ranges::find(ancestors, id) != ancestors.end()) {
      continue;
    }
    ancestors.push_back(id);
    child->Populate(ancestors);
    ancestors.pop_back();
  }
}

Result<std::unique_ptr<NodeStream>> HostNode::Open(Access access) {
  if (read_only() && (access & (Access::kWriteData | Access::kAppendData)) != Access::kNone) {
    return Err<std::unique_ptr<NodeStream>>(MakeError(
        Error::kAccessDenied, std::format("'{}' is on a read only mount", absolute_path_)));
  }
  REX_TRY(std::unique_ptr<HostFile> file, HostFile::OpenExisting(host_path_, access));
  return std::unique_ptr<NodeStream>(std::make_unique<HostStream>(*this, access, std::move(file)));
}

Result<void> HostNode::Truncate() {
  if (read_only() || is_directory()) {
    return Err(
        MakeError(Error::kAccessDenied, std::format("cannot truncate '{}'", absolute_path_)));
  }
  if (auto file = HostFile::OpenExisting(host_path_, Access::kGenericWrite)) {
    REX_TRY_VOID((*file)->SetLength(0));
  } else {
    std::ofstream stream(host_path_, std::ios::binary | std::ios::trunc);
    if (!stream) {
      return Err(
          MakeError(Error::kIO, std::format("cannot truncate '{}'", path_to_utf8(host_path_))));
    }
  }
  std::lock_guard lock(provider().mutex());
  size_ = 0;
  allocation_size_ = 0;
  return Ok();
}

void HostNode::Refresh() {
  const std::optional<HostInfo> info = HostStat(host_path());
  if (!info || info->directory) {
    return;
  }
  std::lock_guard lock(provider().mutex());
  size_ = info->size;
  allocation_size_ = round_up(info->size, u64{kBytesPerSector});
}

bool HostNode::ChildVanished(const Node& child) const {
  std::error_code ec;
  return !std::filesystem::exists(host_path_ / to_path(child.name()), ec);
}

Result<void> HostNode::SetAttributes(FileAttributes attributes) {
  if (read_only()) {
    return Err(MakeError(Error::kAccessDenied,
                         std::format("'{}' is on a read only mount", absolute_path_)));
  }
  std::lock_guard lock(provider().mutex());
  attributes_ = attributes;
  return Ok();
}

Result<void> HostNode::SetTimes(const FileTimes& times) {
  if (read_only()) {
    return Err(MakeError(Error::kAccessDenied,
                         std::format("'{}' is on a read only mount", absolute_path_)));
  }
  std::lock_guard lock(provider().mutex());
  create_time_ = times.create.value_or(create_time_);
  access_time_ = times.access.value_or(access_time_);
  write_time_ = times.write.value_or(write_time_);
  return Ok();
}

Result<std::unique_ptr<Node>> HostNode::CreateChildImpl(std::string_view name,
                                                        FileAttributes attributes) {
  if (!IsHostName(name)) {
    return Err<std::unique_ptr<Node>>(
        MakeError(Error::kInvalidPath, std::format("'{}' is not a valid host name", name)));
  }
  const std::filesystem::path full_path = host_path_ / to_path(name);
  auto refuse = [&] {
    return Err<std::unique_ptr<Node>>(MakeError(
        Error::kAccessDenied, std::format("cannot create '{}'", path_to_utf8(full_path))));
  };
  if (HasFlags(attributes, FileAttributes::kDirectory)) {
    std::error_code ec;
    if (!std::filesystem::create_directory(full_path, ec)) {
      if (!ec) {
        return Err<std::unique_ptr<Node>>(MakeError(
            Error::kExists, std::format("'{}' exists on the host", path_to_utf8(full_path))));
      }
      return refuse();
    }
  } else {
    REX_TRY_VOID(HostCreateNew(full_path));
  }
  const std::optional<HostInfo> info = HostStat(full_path);
  if (!info) {
    return refuse();
  }
  return std::unique_ptr<Node>(Create(host(), *this, *info));
}

Result<void> HostNode::DeleteChildImpl(Node& child) {
  const std::filesystem::path full_path = host_path_ / to_path(child.name());
  auto refuse = [&] {
    return Err(MakeError(Error::kAccessDenied,
                         std::format("cannot delete '{}'", path_to_utf8(full_path))));
  };
  std::error_code ec;
  if (child.is_directory()) {
    const std::uintmax_t removed = std::filesystem::remove_all(full_path, ec);
    if (removed < 1 || removed == static_cast<std::uintmax_t>(-1)) {
      return refuse();
    }
  } else if (std::filesystem::is_directory(full_path, ec) ||
             !std::filesystem::remove(full_path, ec)) {
    return refuse();
  }
  if (std::filesystem::status(full_path, ec).type() != std::filesystem::file_type::not_found) {
    return refuse();
  }
  return Ok();
}

Result<void> HostNode::MoveImpl(Node& new_parent, std::string_view name, bool replace_existing) {
  if (!IsHostName(name)) {
    return Err(MakeError(Error::kInvalidPath, std::format("'{}' is not a valid host name", name)));
  }
  return HostMove(host_path_, static_cast<HostNode&>(new_parent).host_path_ / to_path(name),
                  replace_existing);
}

void HostNode::OnMoved() {
  host_path_ = static_cast<const HostNode*>(parent())->host_path_ / to_path(name());
}

}  // namespace rex::filesystem
