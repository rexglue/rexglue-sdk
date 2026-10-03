/**
 * @file        filesystem/file_system.cpp
 * @brief       The mount table rexfilesystem serves
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/virtual_file_system.cc (Ben Vanik, 2020).
 */

#include "filesystem/file_system.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <system_error>
#include <utility>

#include <rex/filesystem/error.h>
#include <rex/logging.h>
#include <rex/macros.h>
#include <rex/string.h>

#include "filesystem/node_file.h"

namespace rex::filesystem {

namespace {

constexpr std::string_view kGamePartition = "\\Device\\Harddisk0\\Partition1";
constexpr std::string_view kUpdatePartition = "\\Device\\Harddisk0\\PartitionUpdate";
constexpr std::string_view kHarddisk = "\\Device\\Harddisk0";
constexpr size_t kMaxLinkDepth = 64;

Access ExpandAccess(Access access) {
  if (HasFlags(access, Access::kGenericRead)) {
    access |= Access::kReadData;
  }
  if (HasFlags(access, Access::kGenericWrite)) {
    access |= Access::kWriteData;
  }
  if (HasFlags(access, Access::kGenericAll)) {
    access |= Access::kReadData | Access::kWriteData;
  }
  return access;
}

bool StartsWithComponents(std::string_view path, std::string_view prefix) {
  if (!utf8_starts_with_case(path, prefix)) {
    return false;
  }
  return path.size() == prefix.size() || prefix.ends_with('\\') || path[prefix.size()] == '\\';
}

}  // namespace

Result<FileSystem::Resolved> FileSystem::ResolveMount(std::string_view path,
                                                      bool log_missing) const {
  std::string resolved = utf8_canonicalize_guest_path(path);
  for (size_t depth = 0;; ++depth) {
    const auto link = std::ranges::find_if(
        links_, [&](const auto& entry) { return StartsWithComponents(resolved, entry.first); });
    if (link == links_.end()) {
      break;
    }
    if (depth == kMaxLinkDepth) {
      return Err<Resolved>(
          MakeError(Error::kInvalidPath, std::format("links of '{}' do not end", path)));
    }
    resolved = link->second + resolved.substr(link->first.size());
  }
  const auto provider = std::ranges::find_if(providers_, [&](const auto& entry) {
    return StartsWithComponents(resolved, entry->mount_path());
  });
  if (provider == providers_.end()) {
    if (log_missing) {
      REXLOG_ERROR(filesystem, "no mount covers '{}'", path);
    }
    return Err<Resolved>(MakeError(Error::kNoMount, std::format("no mount covers '{}'", path)));
  }
  return Resolved{provider->get(), resolved.substr((*provider)->mount_path().size())};
}

Result<Node*> FileSystem::ResolveNode(std::string_view path) const {
  REX_TRY(Resolved resolved, ResolveMount(path));
  Node* node = resolved.provider->Resolve(resolved.relative);
  if (!node) {
    REXLOG_DEBUG(filesystem, "'{}' is not on mount '{}'", path, resolved.provider->mount_path());
    return Err<Node*>(MakeError(Error::kNotFound, std::format("'{}' does not exist", path)));
  }
  REXLOG_TRACE(filesystem, "'{}' resolves to '{}'", path, node->absolute_path());
  return node;
}

std::unique_lock<std::recursive_mutex> FileSystem::LockMount(std::string_view path) const {
  auto resolved = ResolveMount(path, false);
  if (!resolved) {
    return {};
  }
  return std::unique_lock(resolved->provider->mutex());
}

Result<Node*> FileSystem::CreatePath(Node* root, std::string_view path, FileAttributes attributes) {
  Provider* provider = nullptr;
  std::string relative;
  if (root) {
    relative = path;
  } else {
    REX_TRY(Resolved resolved, ResolveMount(path));
    provider = resolved.provider;
    relative = std::move(resolved.relative);
  }
  const std::vector<std::string_view> parts = utf8_split_path(relative);
  if (parts.empty()) {
    return Err<Node*>(MakeError(Error::kInvalidPath, std::format("'{}' names a mount root", path)));
  }
  Node* parent = root ? root : &provider->root();
  std::string partial;
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    partial = utf8_join_guest_paths(partial, parts[i]);
    Node* child = provider ? provider->Resolve(partial) : parent->GetChild(parts[i]);
    if (!child) {
      REX_TRY(child, parent->CreateChild(parts[i], FileAttributes::kDirectory));
    }
    parent = child;
  }
  return parent->CreateChild(parts.back(), attributes);
}

Result<Opened> FileSystem::OpenFrom(Node* root, std::string_view requested,
                                    const OpenOptions& options) {
  std::lock_guard lock(mutex_);
  const std::string canonical = utf8_canonicalize_guest_path(requested);
  const std::string_view path = canonical;
  const auto mount_lock = root ? std::unique_lock(root->provider().mutex()) : LockMount(path);
  Access access = ExpandAccess(options.access);
  auto not_found = [&] {
    return Err<Opened>(MakeError(Error::kNotFound, std::format("'{}' does not exist", path)));
  };

  Node* parent = nullptr;
  Node* node = nullptr;
  const std::string base = utf8_find_base_guest_path(path);
  if (!base.empty()) {
    if (root) {
      parent = root->ResolvePath(base);
    } else {
      REX_TRY(parent, ResolveNode(base));
    }
    if (!parent) {
      return not_found();
    }
    node = parent->GetChild(utf8_find_name_from_guest_path(path));
  } else if (root) {
    node = root->GetChild(path);
  } else if (auto resolved = ResolveNode(path)) {
    node = *resolved;
  }

  if (node) {
    if (node->is_directory() && options.require_file) {
      return Err<Opened>(MakeError(Error::kIsDirectory, std::format("'{}' is a directory", path)));
    }
    if (parent && parent->ChildVanished(*node)) {
      parent->DropChild(*node);
      node = nullptr;
    }
  }

  switch (options.disposition) {
    case Disposition::kOpen:
    case Disposition::kOverwrite:
      if (!node) {
        return not_found();
      }
      break;
    case Disposition::kCreate:
      if (node) {
        return Err<Opened>(MakeError(Error::kExists, std::format("'{}' exists", path)));
      }
      break;
    default:
      break;
  }

  const bool wants_write =
      HasFlags(access, Access::kWriteData) || HasFlags(access, Access::kAppendData);
  if (wants_write && ((parent && parent->read_only()) || (node && node->read_only()))) {
    REXLOG_WARN(filesystem, "'{}' is read only, opening it for read", path);
    access = Access::kGenericRead | Access::kReadData;
  }

  auto replace = [&](OpenAction replaced) -> Result<OpenAction> {
    if (!node->Truncate()) {
      if (!node->Delete()) {
        return Err<OpenAction>(
            MakeError(Error::kAccessDenied, std::format("cannot replace '{}'", path)));
      }
      node = nullptr;
    }
    return replaced;
  };

  OpenAction action = OpenAction::kCreated;
  if (node) {
    switch (options.disposition) {
      case Disposition::kCreate:
        std::unreachable();
      case Disposition::kSupersede: {
        REX_TRY(action, replace(OpenAction::kSuperseded));
        break;
      }
      case Disposition::kOpen:
      case Disposition::kOpenIf:
        action = OpenAction::kOpened;
        break;
      case Disposition::kOverwrite:
      case Disposition::kOverwriteIf: {
        REX_TRY(action, replace(OpenAction::kOverwritten));
        break;
      }
    }
  }

  if (!node) {
    const FileAttributes attributes =
        options.create_directory ? FileAttributes::kDirectory : FileAttributes::kNormal;
    auto created = CreatePath(root, path, attributes);
    if (!created) {
      return Err<Opened>(MakeError(Error::kAccessDenied, created.error().message));
    }
    node = *created;
  }

  REX_TRY(std::unique_ptr<NodeStream> stream, node->Open(access));
  return Opened{std::make_unique<NodeFile>(*node, access, std::move(stream)), action};
}

Result<void> FileSystem::Mount(const MountEntry& entry) {
  std::lock_guard lock(mutex_);
  if (std::ranges::any_of(providers_,
                          [&](const auto& p) { return p->mount_path() == entry.path; })) {
    return Err(MakeError(Error::kMountExists, std::format("'{}' is mounted", entry.path)));
  }
  REX_TRY(std::unique_ptr<Provider> provider, CreateProvider(entry));
  providers_.push_back(std::move(provider));
  REXLOG_DEBUG(filesystem, "mounted '{}' at '{}'", path_to_utf8(entry.source), entry.path);
  return Ok();
}

Result<void> FileSystem::Unmount(std::string_view path) {
  std::lock_guard lock(mutex_);
  const auto it =
      std::ranges::find_if(providers_, [&](const auto& p) { return p->mount_path() == path; });
  if (it == providers_.end()) {
    return Err(MakeError(Error::kNoMount, std::format("'{}' is not mounted", path)));
  }
  REXLOG_DEBUG(filesystem, "unmounted '{}'", path);
  providers_.erase(it);
  return Ok();
}

std::vector<MountEntry> FileSystem::Mounts() const {
  std::lock_guard lock(mutex_);
  std::vector<MountEntry> result;
  result.reserve(providers_.size());
  for (const auto& provider : providers_) {
    result.push_back(provider->entry());
  }
  return result;
}

Result<void> FileSystem::Link(std::string_view link, std::string_view target) {
  std::lock_guard lock(mutex_);
  if (std::ranges::any_of(links_,
                          [&](const auto& entry) { return utf8_equal_case(entry.first, link); })) {
    return Err(MakeError(Error::kExists, std::format("link '{}' exists", link)));
  }
  links_.emplace_back(link, target);
  REXLOG_DEBUG(filesystem, "linked '{}' to '{}'", link, target);
  return Ok();
}

Result<void> FileSystem::Unlink(std::string_view link) {
  std::lock_guard lock(mutex_);
  const auto it = std::ranges::find_if(
      links_, [&](const auto& entry) { return utf8_equal_case(link, entry.first); });
  if (it == links_.end()) {
    return Err(MakeError(Error::kNotFound, std::format("no link '{}'", link)));
  }
  REXLOG_DEBUG(filesystem, "unlinked '{}' from '{}'", it->first, it->second);
  links_.erase(it);
  return Ok();
}

std::optional<std::string> FileSystem::FindLink(std::string_view path) const {
  std::lock_guard lock(mutex_);
  const auto it = std::ranges::find_if(
      links_, [&](const auto& entry) { return StartsWithComponents(path, entry.first); });
  if (it == links_.end()) {
    return std::nullopt;
  }
  return it->second;
}

Result<void> FileSystem::ConfigureStandardMounts(const PathsConfig& paths) {
  std::lock_guard lock(mutex_);
  Clear();
  std::error_code ec;
  const std::filesystem::path game_root = std::filesystem::absolute(paths.game_data_root, ec);
  if (paths.game_data_root.empty() || !std::filesystem::is_directory(game_root, ec)) {
    return Err(MakeError(Error::kNotFound, std::format("game_data_root '{}' is not a folder",
                                                       path_to_utf8(paths.game_data_root))));
  }
  REX_TRY_VOID(Mount(
      {std::string(kGamePartition), ProviderKind::kHostDirectory, game_root, game_mount_flags_}));
  REX_TRY_VOID(Link("game:", kGamePartition));
  REX_TRY_VOID(Link("d:", kGamePartition));

  if (!paths.update_data_root.empty()) {
    const std::filesystem::path update_root = std::filesystem::absolute(paths.update_data_root, ec);
    if (std::filesystem::exists(update_root, ec)) {
      if (auto mounted = Mount({std::string(kUpdatePartition), ProviderKind::kHostDirectory,
                                update_root, MountFlags::kNone})) {
        REX_TRY_VOID(Link("update:", kUpdatePartition));
      } else {
        REXLOG_WARN(filesystem, "update_data_root is not mounted, {}", mounted.error().message);
      }
    }
  }

  return Mount({std::string(kHarddisk),
                ProviderKind::kNull,
                {},
                MountFlags::kWritable,
                {"\\Partition0", "\\Cache0", "\\Cache1"}});
}

void FileSystem::Clear() {
  std::lock_guard lock(mutex_);
  providers_.clear();
  links_.clear();
}

Result<Opened> FileSystem::Open(std::string_view path, const OpenOptions& options) {
  return OpenFrom(nullptr, path, options);
}

Result<Opened> FileSystem::OpenRelative(IFile& root, std::string_view path,
                                        const OpenOptions& options) {
  auto* file = dynamic_cast<NodeFile*>(&root);
  if (!file) {
    return Err<Opened>(
        MakeError(Error::kInvalidPath, "the root file was not opened by this mount table"));
  }
  return OpenFrom(&file->node(), path, options);
}

Result<FileInfo> FileSystem::Stat(std::string_view path) {
  std::lock_guard lock(mutex_);
  const auto mount_lock = LockMount(path);
  REX_TRY(Node * node, ResolveNode(path));
  return node->Info();
}

Result<std::unique_ptr<IDirectory>> FileSystem::List(std::string_view path,
                                                     std::string_view pattern) {
  std::lock_guard lock(mutex_);
  const auto mount_lock = LockMount(path);
  REX_TRY(Node * node, ResolveNode(path));
  if (!node->is_directory()) {
    return Err<std::unique_ptr<IDirectory>>(
        MakeError(Error::kNotDirectory, std::format("'{}' is not a directory", path)));
  }
  return std::unique_ptr<IDirectory>(std::make_unique<NodeDirectory>(*node, pattern));
}

Result<void> FileSystem::CreateDirectories(std::string_view path) {
  std::lock_guard lock(mutex_);
  const auto mount_lock = LockMount(path);
  if (auto node = ResolveNode(path)) {
    if (!(*node)->is_directory()) {
      return Err(MakeError(Error::kNotDirectory, std::format("'{}' is a file", path)));
    }
    return Ok();
  }
  REX_TRY_VOID(CreatePath(nullptr, path, FileAttributes::kDirectory));
  return Ok();
}

Result<void> FileSystem::Remove(std::string_view path) {
  std::lock_guard lock(mutex_);
  const auto mount_lock = LockMount(path);
  REX_TRY(Node * node, ResolveNode(path));
  return node->Delete();
}

Result<void> FileSystem::Rename(std::string_view from, std::string_view to) {
  std::lock_guard lock(mutex_);
  const auto mount_lock = LockMount(from);
  REX_TRY(Node * node, ResolveNode(from));
  return node->Rename(to);
}

Result<VolumeInfo> FileSystem::Volume(std::string_view path) {
  std::lock_guard lock(mutex_);
  REX_TRY(Resolved resolved, ResolveMount(path));
  return resolved.provider->Volume();
}

}  // namespace rex::filesystem
