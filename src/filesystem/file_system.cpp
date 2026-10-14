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
          BuildError(FileSystemError::kInvalidPath, std::format("links of '{}' do not end", path)));
    }
    resolved = link->second + resolved.substr(link->first.size());
  }
  const auto provider = std::ranges::find_if(providers_, [&](const auto& entry) {
    return StartsWithComponents(resolved, entry->GetMountPath());
  });
  if (provider == providers_.end()) {
    if (log_missing) {
      REXLOG_ERROR(filesystem, "no mount covers '{}'", path);
    }
    return Err<Resolved>(
        BuildError(FileSystemError::kNoMount, std::format("no mount covers '{}'", path)));
  }
  return Resolved{provider->get(), resolved.substr((*provider)->GetMountPath().size())};
}

Result<Node*> FileSystem::ResolveNode(std::string_view path) const {
  REX_TRY(Resolved resolved, ResolveMount(path));
  Node* node = resolved.provider->Resolve(resolved.relative);
  if (!node) {
    REXLOG_DEBUG(filesystem, "'{}' is not on mount '{}'", path, resolved.provider->GetMountPath());
    return Err<Node*>(
        BuildError(FileSystemError::kNotFound, std::format("'{}' does not exist", path)));
  }
  REXLOG_TRACE(filesystem, "'{}' resolves to '{}'", path, node->GetAbsolutePath());
  return node;
}

std::unique_lock<std::recursive_mutex> FileSystem::LockMount(std::string_view path) const {
  auto resolved = ResolveMount(path, false);
  if (!resolved) {
    return {};
  }
  return std::unique_lock(resolved->provider->GetMutex());
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
    return Err<Node*>(
        BuildError(FileSystemError::kInvalidPath, std::format("'{}' names a mount root", path)));
  }
  Node* parent = root ? root : &provider->GetRoot();
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
                                    const OpenOptions& options, bool retried) {
  std::lock_guard lock(mutex_);
  const std::string canonical = utf8_canonicalize_guest_path(requested);
  const std::string_view path = canonical;
  const auto mount_lock = root ? std::unique_lock(root->GetProvider().GetMutex()) : LockMount(path);
  Access access = ExpandAccess(options.access);
  auto not_found = [&] {
    return Err<Opened>(
        BuildError(FileSystemError::kNotFound, std::format("'{}' does not exist", path)));
  };

  Node* parent = nullptr;
  Node* node = nullptr;
  const std::string base = utf8_find_base_guest_path(path);
  if (root) {
    Provider& provider = root->GetProvider();
    parent = base.empty() ? root : provider.Resolve(utf8_join_guest_paths(root->GetPath(), base));
    if (!parent) {
      return not_found();
    }
    node = provider.Resolve(utf8_join_guest_paths(root->GetPath(), path));
  } else {
    if (!base.empty()) {
      REX_TRY(parent, ResolveNode(base));
    }
    if (auto resolved = ResolveNode(path)) {
      node = *resolved;
    }
  }

  if (node) {
    if (node->IsDirectory() && options.require_file) {
      return Err<Opened>(
          BuildError(FileSystemError::kIsDirectory, std::format("'{}' is a directory", path)));
    }
    if (parent && parent->HasChildVanished(*node)) {
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
        return Err<Opened>(BuildError(FileSystemError::kExists, std::format("'{}' exists", path)));
      }
      break;
    default:
      break;
  }

  const bool wants_write =
      HasFlags(access, Access::kWriteData) || HasFlags(access, Access::kAppendData);
  if (wants_write && ((parent && parent->IsReadOnly()) || (node && node->IsReadOnly()))) {
    REXLOG_WARN(filesystem, "'{}' is read only, opening it for read", path);
    access = Access::kGenericRead | Access::kReadData;
  }

  auto replace = [&](OpenAction replaced) -> Result<OpenAction> {
    if (!node->Truncate()) {
      if (!node->Delete()) {
        return Err<OpenAction>(
            BuildError(FileSystemError::kAccessDenied, std::format("cannot replace '{}'", path)));
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
      const bool exists = CodeOf(created.error()) == FileSystemError::kExists;
      if (exists && options.disposition != Disposition::kCreate && !retried) {
        return OpenFrom(root, requested, options, true);
      }
      return Err<Opened>(
          BuildError(exists ? FileSystemError::kExists : FileSystemError::kAccessDenied,
                     created.error().message));
    }
    node = *created;
  }

  REX_TRY(std::unique_ptr<NodeStream> stream, node->Open(access));
  return Opened{std::make_unique<NodeFile>(*node, access, std::move(stream)), action};
}

Result<void> FileSystem::Mount(const MountEntry& entry) {
  std::lock_guard lock(mutex_);
  if (std::ranges::any_of(providers_,
                          [&](const auto& p) { return p->GetMountPath() == entry.path; })) {
    return Err(
        BuildError(FileSystemError::kMountExists, std::format("'{}' is mounted", entry.path)));
  }
  REX_TRY(std::unique_ptr<Provider> provider, CreateProvider(entry));
  providers_.push_back(std::move(provider));
  REXLOG_DEBUG(filesystem, "mounted '{}' at '{}'", PathToUTF8(entry.source), entry.path);
  return Ok();
}

Result<void> FileSystem::Unmount(std::string_view path) {
  std::lock_guard lock(mutex_);
  const auto it =
      std::ranges::find_if(providers_, [&](const auto& p) { return p->GetMountPath() == path; });
  if (it == providers_.end()) {
    return Err(BuildError(FileSystemError::kNoMount, std::format("'{}' is not mounted", path)));
  }
  REXLOG_DEBUG(filesystem, "unmounted '{}'", path);
  providers_.erase(it);
  return Ok();
}

std::vector<MountEntry> FileSystem::GetMounts() const {
  std::lock_guard lock(mutex_);
  std::vector<MountEntry> result;
  result.reserve(providers_.size());
  for (const auto& provider : providers_) {
    result.push_back(provider->GetEntry());
  }
  return result;
}

Result<void> FileSystem::Link(std::string_view link, std::string_view target) {
  std::lock_guard lock(mutex_);
  if (std::ranges::any_of(links_,
                          [&](const auto& entry) { return utf8_equal_case(entry.first, link); })) {
    return Err(BuildError(FileSystemError::kExists, std::format("link '{}' exists", link)));
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
    return Err(BuildError(FileSystemError::kNotFound, std::format("no link '{}'", link)));
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
    return Err(BuildError(
        FileSystemError::kNotFound,
        std::format("game_data_root '{}' is not a folder", PathToUTF8(paths.game_data_root))));
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
    return Err<Opened>(BuildError(FileSystemError::kInvalidPath,
                                  "the root file was not opened by this mount table"));
  }
  return OpenFrom(&file->GetNode(), path, options);
}

Result<FileInfo> FileSystem::Stat(std::string_view path) {
  std::lock_guard lock(mutex_);
  const auto mount_lock = LockMount(path);
  REX_TRY(Node * node, ResolveNode(path));
  return node->GetInfo();
}

Result<std::unique_ptr<IDirectory>> FileSystem::List(std::string_view path,
                                                     std::string_view pattern) {
  std::lock_guard lock(mutex_);
  const auto mount_lock = LockMount(path);
  REX_TRY(Node * node, ResolveNode(path));
  if (!node->IsDirectory()) {
    return Err<std::unique_ptr<IDirectory>>(
        BuildError(FileSystemError::kNotDirectory, std::format("'{}' is not a directory", path)));
  }
  return std::unique_ptr<IDirectory>(std::make_unique<NodeDirectory>(*node, pattern));
}

Result<void> FileSystem::CreateDirectories(std::string_view path) {
  std::lock_guard lock(mutex_);
  const auto mount_lock = LockMount(path);
  if (auto node = ResolveNode(path)) {
    if (!(*node)->IsDirectory()) {
      return Err(BuildError(FileSystemError::kNotDirectory, std::format("'{}' is a file", path)));
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

Result<void> FileSystem::Rename(std::string_view from, std::string_view to, bool replace_existing) {
  std::lock_guard lock(mutex_);
  const auto mount_lock = LockMount(from);
  REX_TRY(Node * node, ResolveNode(from));
  return Move(*node, to, replace_existing, nullptr);
}

Result<void> FileSystem::Rename(IFile& file, std::string_view to, bool replace_existing,
                                IFile* root) {
  std::lock_guard lock(mutex_);
  Node& node = static_cast<NodeFile&>(file).GetNode();
  const std::unique_lock mount_lock(node.GetProvider().GetMutex());
  return Move(node, to, replace_existing,
              root ? &static_cast<NodeFile*>(root)->GetNode() : nullptr);
}

Result<void> FileSystem::Move(Node& node, std::string_view to, bool replace_existing, Node* root) {
  const std::string target = utf8_canonicalize_guest_path(to);
  const std::string base = utf8_find_base_guest_path(target);
  const std::string name(utf8_find_name_from_guest_path(target));
  Node* parent = nullptr;
  if (root) {
    parent = base.empty()
                 ? root
                 : root->GetProvider().Resolve(utf8_join_guest_paths(root->GetPath(), base));
  } else if (base.empty()) {
    parent = node.GetParent();
  } else {
    REX_TRY(Resolved resolved, ResolveMount(base));
    if (resolved.provider != &node.GetProvider()) {
      return Err(
          BuildError(FileSystemError::kNotSameDevice,
                     std::format("'{}' is not on the mount of '{}'", to, node.GetAbsolutePath())));
    }
    parent = resolved.provider->Resolve(resolved.relative);
  }
  if (root && &root->GetProvider() != &node.GetProvider()) {
    return Err(
        BuildError(FileSystemError::kNotSameDevice,
                   std::format("'{}' is not on the mount of '{}'", to, node.GetAbsolutePath())));
  }
  if (!parent || !parent->IsDirectory()) {
    return Err(BuildError(FileSystemError::kNotFound,
                          std::format("the folder of '{}' does not exist", to)));
  }
  if (name.empty()) {
    return Err(BuildError(FileSystemError::kInvalidPath, std::format("'{}' has no name", to)));
  }
  return node.MoveTo(*parent, name, replace_existing);
}

Result<VolumeInfo> FileSystem::Volume(std::string_view path) {
  std::lock_guard lock(mutex_);
  REX_TRY(Resolved resolved, ResolveMount(path));
  return resolved.provider->GetVolume();
}

}  // namespace rex::filesystem
