/**
 * @file        filesystem/file_system.h
 * @brief       The mount table rexfilesystem serves
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/virtual_file_system.h (Ben Vanik, 2013).
 */

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/filesystem/file_system.h>

#include "filesystem/provider.h"

namespace rex::filesystem {

/** The mount table. */
class FileSystem final : public IFileSystem {
 public:
  /**
   * Sets the flags ConfigureStandardMounts gives the game mount.
   *
   * @param flags The game mount flags from the filesystem slice.
   */
  void set_game_mount_flags(MountFlags flags) { game_mount_flags_ = flags; }

  [[nodiscard]] Result<void> Mount(const MountEntry& entry) override;
  [[nodiscard]] Result<void> Unmount(std::string_view path) override;
  [[nodiscard]] std::vector<MountEntry> Mounts() const override;
  [[nodiscard]] Result<void> Link(std::string_view link, std::string_view target) override;
  [[nodiscard]] Result<void> Unlink(std::string_view link) override;
  [[nodiscard]] std::optional<std::string> FindLink(std::string_view path) const override;
  [[nodiscard]] Result<void> ConfigureStandardMounts(const PathsConfig& paths) override;
  void Clear() override;
  [[nodiscard]] Result<Opened> Open(std::string_view path, const OpenOptions& options) override;
  [[nodiscard]] Result<Opened> OpenRelative(IFile& root, std::string_view path,
                                            const OpenOptions& options) override;
  [[nodiscard]] Result<FileInfo> Stat(std::string_view path) override;
  [[nodiscard]] Result<std::unique_ptr<IDirectory>> List(std::string_view path,
                                                         std::string_view pattern) override;
  [[nodiscard]] Result<void> CreateDirectories(std::string_view path) override;
  [[nodiscard]] Result<void> Remove(std::string_view path) override;
  [[nodiscard]] Result<void> Rename(std::string_view from, std::string_view to,
                                    bool replace_existing) override;
  [[nodiscard]] Result<void> Rename(IFile& file, std::string_view to, bool replace_existing,
                                    IFile* root = nullptr) override;
  [[nodiscard]] Result<VolumeInfo> Volume(std::string_view path) override;

 private:
  [[nodiscard]] Result<void> Move(Node& node, std::string_view to, bool replace_existing,
                                  Node* root);
  struct Resolved {
    Provider* provider = nullptr;
    std::string relative;
  };

  [[nodiscard]] Result<Resolved> ResolveMount(std::string_view path, bool log_missing = true) const;
  [[nodiscard]] Result<Node*> ResolveNode(std::string_view path) const;
  [[nodiscard]] std::unique_lock<std::recursive_mutex> LockMount(std::string_view path) const;
  [[nodiscard]] Result<Node*> CreatePath(Node* root, std::string_view path,
                                         FileAttributes attributes);
  [[nodiscard]] Result<Opened> OpenFrom(Node* root, std::string_view path,
                                        const OpenOptions& options, bool retried = false);

  mutable std::recursive_mutex mutex_;
  std::vector<std::shared_ptr<Provider>> providers_;
  std::vector<std::pair<std::string, std::string>> links_;
  MountFlags game_mount_flags_ = MountFlags::kNone;
};

}  // namespace rex::filesystem
