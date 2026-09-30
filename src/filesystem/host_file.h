/**
 * @file        filesystem/host_file.h
 * @brief       Positional host file handle and host directory queries of the host provider
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/filesystem_win.cc and filesystem_posix.cc
 *              (Ben Vanik, 2020).
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <rex/filesystem/open.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::filesystem {

/** Host view of 1 file or folder. */
struct HostInfo {
  std::filesystem::path name; /**< Last path component. */
  bool directory = false;     /**< True for a folder. */
  u64 size = 0;               /**< Size in bytes, 0 for a folder. */
  u64 create_time = 0;        /**< Creation time in FILETIME ticks. */
  u64 access_time = 0;        /**< Last access time in FILETIME ticks. */
  u64 write_time = 0;         /**< Last write time in FILETIME ticks. */
};

/** An open host file read and written at explicit offsets. */
class HostFile : public NonCopyable {
 public:
  /**
   * Opens a file that exists.
   *
   * @param path The host file.
   * @param access The requested access bits.
   * @param share_delete True when the file may be deleted or replaced while open.
   * @return The handle, or kNotFound when the host refuses.
   */
  [[nodiscard]] static Result<std::unique_ptr<HostFile>> OpenExisting(
      const std::filesystem::path& path, Access access, bool share_delete);

  ~HostFile();

  /**
   * Returns the host file.
   *
   * @return The path the handle was opened with.
   */
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

  /**
   * Copies bytes from the file.
   *
   * @param offset Byte offset in the file.
   * @param buffer Destination.
   * @return The bytes read, 0 at the end, or kIO.
   */
  [[nodiscard]] Result<size_t> Read(u64 offset, std::span<u8> buffer);

  /**
   * Writes bytes into the file.
   *
   * @param offset Byte offset in the file.
   * @param buffer Source bytes.
   * @return The bytes written, or kIO.
   */
  [[nodiscard]] Result<size_t> Write(u64 offset, std::span<const u8> buffer);

  /**
   * Changes the file size.
   *
   * @param length The new size in bytes.
   * @return kIO when the host refuses.
   */
  [[nodiscard]] Result<void> SetLength(u64 length);

  /** Writes pending host buffers to the device. */
  void Flush() const;

 private:
  HostFile(std::filesystem::path path, std::intptr_t native);

  std::filesystem::path path_;
  std::intptr_t native_;
};

/**
 * Returns the host view of a path.
 *
 * @param path A host path.
 * @return The information, or empty when the path does not exist.
 */
[[nodiscard]] std::optional<HostInfo> HostStat(const std::filesystem::path& path);

/**
 * Returns the children of a host folder.
 *
 * @param path A host folder.
 * @return Every child except dot and dot dot, empty when the folder cannot be read.
 */
[[nodiscard]] std::vector<HostInfo> HostList(const std::filesystem::path& path);

}  // namespace rex::filesystem
