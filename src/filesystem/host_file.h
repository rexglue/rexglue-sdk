/**
 * @file        filesystem/host_file.h
 * @brief       Positional host file handle and host queries
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

#include <rex/filesystem/file_open.h>
#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::filesystem {

#if REX_PLATFORM_WIN32
/** Host file handle, a HANDLE. */
using NativeHandle = void*;
#else
/** Host file handle, a file descriptor. */
using NativeHandle = int;
#endif

/** Host view of a file or folder. */
struct HostFileStat {
  std::filesystem::path name; /**< Last path component. */
  bool directory = false;
  u64 size = 0;        /**< Size in bytes, 0 for a folder. */
  u64 create_time = 0; /**< Creation time in FILETIME ticks. */
  u64 access_time = 0; /**< Last access time in FILETIME ticks. */
  u64 write_time = 0;  /**< Last write time in FILETIME ticks. */
  u64 device = 0;      /**< Host device ID, 0 where the host reports none. */
  u64 inode = 0;       /**< Host file ID on the device, 0 where the host reports none. */
};

/** An open host file read and written at offsets. */
class HostFile : public NonCopyable {
  struct Key {};

 public:
  /**
   * The file stays free to be moved or deleted while open.
   *
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
   * @param native The open host handle, closed by the destructor.
   */
  HostFile(Key key, std::filesystem::path path, NativeHandle native);

  ~HostFile();

  /** Returns the path the handle was opened with. */
  [[nodiscard]] const std::filesystem::path& GetPath() const { return path_; }

  /**
   * @param offset Byte offset in the file.
   * @param buffer Destination.
   * @return The bytes read, 0 at the end, or kIO.
   */
  [[nodiscard]] Result<size_t> Read(u64 offset, std::span<u8> buffer);

  /**
   * @param offset Byte offset in the file.
   * @param buffer Source bytes.
   * @return The bytes written, or kIO.
   */
  [[nodiscard]] Result<size_t> Write(u64 offset, std::span<const u8> buffer);

  /**
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

/** Returns the host view of a path, or empty when the path does not exist. */
[[nodiscard]] std::optional<HostFileStat> HostStat(const std::filesystem::path& path);

/**
 * Returns the children of a host folder. It omits dot and dot dot and is empty when the folder
 * cannot be read.
 */
[[nodiscard]] std::vector<HostFileStat> HostList(const std::filesystem::path& path);

}  // namespace rex::filesystem
