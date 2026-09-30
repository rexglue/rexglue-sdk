/**
 * @file        filesystem/file_system.h
 * @brief       The mount table interface, served by rexfilesystem as rex.filesystem version 1
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/filesystem/directory.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/info.h>
#include <rex/filesystem/mount.h>
#include <rex/filesystem/open.h>
#include <rex/macros.h>
#include <rex/paths.h>
#include <rex/result.h>

namespace rex::filesystem {

/**
 * The mount table every guest path resolves through. A path is canonicalized, links are substituted
 * by case insensitive prefix until none matches, and the first mount in registration order whose
 * path is a case insensitive prefix owns the rest. Every method takes the table mutex.
 */
class IFileSystem {
 public:
  REX_INTERFACE(IFileSystem, "rex.filesystem", 1);
  virtual ~IFileSystem() = default;

  /**
   * Appends a mount.
   *
   * @param entry The mount description.
   * @return kMountExists for a taken path, kNotFound when the source is missing, kCorrupt for a
   *         container that fails validation.
   */
  [[nodiscard]] virtual Result<void> Mount(const MountEntry& entry) = 0;

  /**
   * Removes the mount with exactly this path. Open files of the mount must be closed first.
   *
   * @param path The mount path.
   * @return kNoMount when no mount has the path.
   */
  [[nodiscard]] virtual Result<void> Unmount(std::string_view path) = 0;

  /**
   * Returns the mounts in registration order.
   *
   * @return The mount descriptions.
   */
  [[nodiscard]] virtual std::vector<MountEntry> Mounts() const = 0;

  /**
   * Adds a link.
   *
   * @param link The link name, such as game colon.
   * @param target The path the link stands for.
   * @return kExists when a link with exactly this name exists. The first link stays.
   */
  [[nodiscard]] virtual Result<void> Link(std::string_view link, std::string_view target) = 0;

  /**
   * Removes a link, case insensitive.
   *
   * @param link The link name.
   * @return kNotFound when no link has the name.
   */
  [[nodiscard]] virtual Result<void> Unlink(std::string_view link) = 0;

  /**
   * Returns the target of the first link that is a case insensitive prefix of path.
   *
   * @param path A guest path.
   * @return The unsubstituted target, or empty when no link matches.
   */
  [[nodiscard]] virtual std::optional<std::string> FindLink(std::string_view path) const = 0;

  /**
   * Replaces the table with the standard mounts. The game partition with game and d links, the
   * update partition with an update link when update_data_root exists, and the null mount of
   * Harddisk0 for Partition0, Cache0 and Cache1.
   *
   * @param paths The host paths. game_data_root is required.
   * @return kNotFound when game_data_root is empty or not a folder.
   */
  [[nodiscard]] virtual Result<void> ConfigureStandardMounts(const PathsConfig& paths) = 0;

  /** Removes every mount and every link. */
  virtual void Clear() = 0;

  /**
   * Opens or creates a file or directory.
   *
   * @param path A guest path.
   * @param options Disposition, access and kind.
   * @return The file and the action, or the error. A write request on a read only mount is
   *         downgraded to read access.
   */
  [[nodiscard]] virtual Result<Opened> Open(std::string_view path, const OpenOptions& options) = 0;

  /**
   * Opens or creates a file or directory relative to an open directory.
   *
   * @param root An open directory of this table.
   * @param path A path relative to root.
   * @param options Disposition, access and kind.
   * @return The file and the action, or the error.
   */
  [[nodiscard]] virtual Result<Opened> OpenRelative(IFile& root, std::string_view path,
                                                    const OpenOptions& options) = 0;

  /**
   * Returns the information of a path without opening it.
   *
   * @param path A guest path.
   * @return The information, kNoMount or kNotFound.
   */
  [[nodiscard]] virtual Result<FileInfo> Stat(std::string_view path) = 0;

  /**
   * Starts a walk of the children of a directory.
   *
   * @param path A guest path naming a directory.
   * @param pattern The wildcard pattern, empty for every child.
   * @return The walk, kNotFound or kNotDirectory.
   */
  [[nodiscard]] virtual Result<std::unique_ptr<IDirectory>> List(std::string_view path,
                                                                 std::string_view pattern) = 0;

  /**
   * Creates a directory and every missing parent.
   *
   * @param path A guest path.
   * @return Ok when the directory exists after the call, kAccessDenied on a read only mount.
   */
  [[nodiscard]] virtual Result<void> CreateDirectories(std::string_view path) = 0;

  /**
   * Deletes a file, or a directory with its contents.
   *
   * @param path A guest path.
   * @return kNotFound, or kAccessDenied for a mount root or a read only mount.
   */
  [[nodiscard]] virtual Result<void> Remove(std::string_view path) = 0;

  /**
   * Moves a file or directory inside its mount.
   *
   * @param from The guest path to move.
   * @param to The new guest path, first component dropped as in IFile::Rename.
   * @return kNotFound, or the provider error.
   */
  [[nodiscard]] virtual Result<void> Rename(std::string_view from, std::string_view to) = 0;

  /**
   * Returns the volume information of the mount that owns a path.
   *
   * @param path A guest path.
   * @return The information, or kNoMount.
   */
  [[nodiscard]] virtual Result<VolumeInfo> Volume(std::string_view path) = 0;
};

}  // namespace rex::filesystem
