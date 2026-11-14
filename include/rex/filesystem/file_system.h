/**
 * @file        filesystem/file_system.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/filesystem/directory.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/file_open.h>
#include <rex/filesystem/file_stat.h>
#include <rex/filesystem/file_system_error.h>
#include <rex/filesystem/mount.h>
#include <rex/memory/host.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::filesystem {

inline constexpr memory::Tag kFilesystem{7};

/** The mount table guest paths resolve through. The first matching mount owns a path. */
class IFileSystem : public NonCopyable {
 public:
  virtual ~IFileSystem() = default;

  /**
   * Appends a mount.
   *
   * @return kMountExists for a taken path, kNotFound for a missing source, kCorrupt for a bad
   *         container.
   */
  [[nodiscard]] virtual Result<void> Mount(const MountEntry& entry) = 0;

  /**
   * Removes the mount with exactly this path. Its open files must be closed first.
   *
   * @return kNoMount when no mount has the path.
   */
  [[nodiscard]] virtual Result<void> Unmount(std::string_view path) = 0;

  [[nodiscard]] virtual std::vector<MountEntry> GetMounts() const = 0;

  /**
   * Adds a link.
   *
   * @return kExists when the name is taken.
   */
  [[nodiscard]] virtual Result<void> Link(std::string_view link, std::string_view target) = 0;

  /**
   * Removes a link, case insensitive.
   *
   * @return kNotFound when no link has the name.
   */
  [[nodiscard]] virtual Result<void> Unlink(std::string_view link) = 0;

  /**
   * Returns the target of the first link that is a prefix of path, case insensitive, empty when no
   * link matches.
   */
  [[nodiscard]] virtual std::optional<std::string> FindLink(std::string_view path) const = 0;

  /**
   * Replaces the table with the standard game, update and Harddisk0 mounts.
   *
   * @param game_root The host folder of the game files.
   * @param update_root The host folder of the title update files, empty for none.
   * @return kNotFound when game_root is empty or not a folder.
   */
  [[nodiscard]] virtual Result<void> ConfigureStandardMounts(
      const std::filesystem::path& game_root, const std::filesystem::path& update_root,
      MountFlags game_flags) = 0;

  /** Removes every mount and every link. */
  virtual void Clear() = 0;

  /**
   * Opens or creates a file or directory.
   *
   * @param options Disposition, access and type.
   * @return The file and the action, kPathNotFound when a folder of the path is missing or is a
   *         file, kNotFound when a disposition that needs the file finds none. A write request on a
   *         read only mount gets read access.
   */
  [[nodiscard]] virtual Result<Opened> Open(std::string_view path, const OpenOptions& options) = 0;

  /**
   * Opens or creates a file or directory relative to an open directory.
   *
   * @param options Disposition, access and type.
   * @return The file and the action, or the error.
   */
  [[nodiscard]] virtual Result<Opened> OpenRelative(IFile& root, std::string_view path,
                                                    const OpenOptions& options) = 0;

  /** Returns the information of a path without opening it, kNoMount, kPathNotFound or kNotFound. */
  [[nodiscard]] virtual Result<FileStat> Stat(std::string_view path) = 0;

  /**
   * Starts a walk of the children of a directory.
   *
   * @param pattern The wildcard pattern, empty for every child.
   * @return The walk, kPathNotFound, kNotFound or kNotDirectory.
   */
  [[nodiscard]] virtual Result<std::unique_ptr<IDirectory>> List(std::string_view path,
                                                                 std::string_view pattern) = 0;

  /**
   * Creates a directory and every missing parent.
   *
   * @return Ok when the directory exists after the call, kAccessDenied on a read only mount.
   */
  [[nodiscard]] virtual Result<void> CreateDirectories(std::string_view path) = 0;

  /**
   * Deletes a file, or a directory with its contents.
   *
   * @return kPathNotFound, kNotFound, or kAccessDenied for a mount root or a read only mount.
   */
  [[nodiscard]] virtual Result<void> Remove(std::string_view path) = 0;

  /**
   * Moves a file or directory inside its mount.
   *
   * @param to The new guest path, resolved through links and mounts.
   * @param replace_existing True to replace a closed file at the new path.
   * @return kPathNotFound, kNotFound, kExists, kNotSameDevice, kInvalidPath, kAccessDenied, or the
   *         provider error.
   */
  [[nodiscard]] virtual Result<void> Rename(std::string_view from, std::string_view to,
                                            bool replace_existing) = 0;

  /**
   * Moves an open file or directory inside its mount.
   *
   * @param to The new path. A bare name with no root stays in the folder of the file.
   * @param replace_existing True to replace a closed file at the new path.
   * @param root An open directory the new path is relative to, or null.
   * @return kPathNotFound, kNotFound, kExists, kNotSameDevice, kInvalidPath, kAccessDenied, or the
   *         provider error.
   */
  [[nodiscard]] virtual Result<void> Rename(IFile& file, std::string_view to, bool replace_existing,
                                            IFile* root = nullptr) = 0;

  /** Returns the volume of the mount that owns a path, or kNoMount. */
  [[nodiscard]] virtual Result<VolumeStat> GetVolume(std::string_view path) = 0;
};

}  // namespace rex::filesystem
