/**
 * @file        filesystem/file.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <span>
#include <string_view>

#include <rex/filesystem/directory.h>
#include <rex/filesystem/file_open.h>
#include <rex/filesystem/file_stat.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::filesystem {

/** An open file or directory. Handles on one path share a node. */
class IFile : public NonCopyable {
 public:
  virtual ~IFile() = default;

  [[nodiscard]] virtual FileStat GetInfo() const = 0;

  /** Reads the node size again from its provider. */
  virtual void Refresh() = 0;

  /** Returns the access the file was opened with, generic bits expanded to data bits. */
  [[nodiscard]] virtual Access GetAccess() const = 0;

  [[nodiscard]] virtual VolumeStat GetVolume() const = 0;

  /**
   * Copies bytes of the file.
   *
   * @param offset Byte offset in the file.
   * @return The bytes copied, kAccessDenied without read access, or kEndOfFile.
   */
  [[nodiscard]] virtual Result<size_t> Read(std::span<u8> buffer, u64 offset) = 0;

  /**
   * Writes bytes into the file.
   *
   * @param buffer Source bytes.
   * @param offset Byte offset in the file.
   * @return The bytes written, or kAccessDenied without write access.
   */
  [[nodiscard]] virtual Result<size_t> Write(std::span<const u8> buffer, u64 offset) = 0;

  /**
   * Changes the file size.
   *
   * @param length The new size in bytes.
   * @return kAccessDenied without write access, kUnsupported when the provider cannot.
   */
  [[nodiscard]] virtual Result<void> SetLength(u64 length) = 0;

  [[nodiscard]] virtual Result<void> Flush() = 0;

  [[nodiscard]] virtual Result<void> SetAttributes(FileAttributes attributes) = 0;

  /**
   * Changes the times of the node. An empty field keeps its time.
   *
   * @return kAccessDenied on a read only mount.
   */
  [[nodiscard]] virtual Result<void> SetTimes(const FileTimes& times) = 0;

  virtual void SetDeleteOnClose(bool delete_on_close) = 0;

  /**
   * Starts a walk of the children of this directory.
   *
   * @param pattern The wildcard pattern, empty for every child.
   * @return The walk, or kNotDirectory for a file.
   */
  [[nodiscard]] virtual Result<std::unique_ptr<IDirectory>> List(std::string_view pattern) = 0;

  /** Returns the whole file as bytes without a copy, or kUnsupported. */
  [[nodiscard]] virtual Result<std::span<const u8>> View() = 0;
};

struct Opened {
  std::unique_ptr<IFile> file;
  OpenAction action = OpenAction::kOpened;
};

}  // namespace rex::filesystem
