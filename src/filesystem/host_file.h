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

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <rex/filesystem/open.h>
#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::filesystem {

#if REX_PLATFORM_WIN32
/** Host file handle, a HANDLE. */
using NativeHandle = void*;
#else
/** Host file handle, a file descriptor. */
using NativeHandle = int;
#endif

/** Host view of 1 file or folder. */
struct HostInfo {
  std::filesystem::path name; /**< Last path component. */
  bool directory = false;     /**< True for a folder. */
  u64 size = 0;               /**< Size in bytes, 0 for a folder. */
  u64 create_time = 0;        /**< Creation time in FILETIME ticks. */
  u64 access_time = 0;        /**< Last access time in FILETIME ticks. */
  u64 write_time = 0;         /**< Last write time in FILETIME ticks. */
  u64 device = 0;             /**< Host device id, 0 where the host reports none. */
  u64 inode = 0;              /**< Host file id on the device, 0 where the host reports none. */
};

/** An open host file read and written at explicit offsets. */
class HostFile : public NonCopyable {
  struct Key {};

 public:
  /**
   * Opens a file that exists. The file stays free to be moved or deleted while open.
   *
   * @param path The host file.
   * @param access The requested access bits.
   * @return The handle, kNotFound when the file is missing, kSharingViolation when another handle
   *         refuses sharing, kAccessDenied when the host refuses access, or kIO.
   */
  [[nodiscard]] static Result<std::unique_ptr<HostFile>> OpenExisting(
      const std::filesystem::path& path, Access access);

  /**
   * Wraps an open host handle. Only OpenExisting can make the key.
   *
   * @param key The private construction key.
   * @param path The host file.
   * @param native The open host handle, closed by the destructor.
   */
  HostFile(Key key, std::filesystem::path path, NativeHandle native);

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

  /**
   * Writes pending host buffers to the device.
   *
   * @return kIO when the host refuses.
   */
  [[nodiscard]] Result<void> Flush() const;

 private:
  std::filesystem::path path_;
  NativeHandle native_;
};

/**
 * Creates an empty host file that does not exist yet.
 *
 * @param path The host file.
 * @return kExists when the path exists, kAccessDenied when the host refuses, or kIO.
 */
[[nodiscard]] Result<void> HostCreateNew(const std::filesystem::path& path);

/**
 * Moves a host file or folder.
 *
 * @param from The host path to move.
 * @param to The new host path.
 * @param replace_existing True to replace a file at the new path.
 * @return kExists when the new path exists and replace_existing is false, kAccessDenied, or kIO.
 */
[[nodiscard]] Result<void> HostMove(const std::filesystem::path& from,
                                    const std::filesystem::path& to, bool replace_existing);

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
