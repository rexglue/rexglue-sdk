/**
 * @file        filesystem/file.h
 * @brief       An open file or directory of the mount table
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
#include <rex/filesystem/info.h>
#include <rex/filesystem/open.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::filesystem {

/**
 * An open file or directory. Every handle on the same path shares 1 tree node, so each sees the
 * same size and times. The file closes when the object is destroyed.
 */
class IFile {
 public:
  virtual ~IFile() = default;

  /**
   * Returns the shared node information.
   *
   * @return The name, paths, attributes, sizes and times.
   */
  [[nodiscard]] virtual FileInfo Info() const = 0;

  /** Reads the size of the node again from its provider. */
  virtual void Refresh() = 0;

  /**
   * Returns the access the file was opened with, generic bits expanded to data bits.
   *
   * @return The access bits.
   */
  [[nodiscard]] virtual Access access() const = 0;

  /**
   * Returns the volume the file lives on.
   *
   * @return The volume information of the mount.
   */
  [[nodiscard]] virtual VolumeInfo Volume() const = 0;

  /**
   * Copies bytes of the file.
   *
   * @param buffer Destination. At most buffer.size() bytes are copied.
   * @param offset Byte offset in the file.
   * @return The bytes copied, kAccessDenied without read access, kEndOfFile when the provider
   *         reports the end.
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
   * @return kAccessDenied without write access, kUnsupported when the provider has no size change.
   */
  [[nodiscard]] virtual Result<void> SetLength(u64 length) = 0;

  /**
   * Writes buffered data of the file to its device.
   *
   * @return kIO when the host refuses. Ok on a provider with nothing to write.
   */
  [[nodiscard]] virtual Result<void> Flush() = 0;

  /**
   * Replaces the attribute bits of the node.
   *
   * @param attributes The new bits.
   * @return kAccessDenied on a read only mount.
   */
  [[nodiscard]] virtual Result<void> SetAttributes(FileAttributes attributes) = 0;

  /**
   * Changes the times of the node. An empty field keeps its time.
   *
   * @param times The new times.
   * @return kAccessDenied on a read only mount.
   */
  [[nodiscard]] virtual Result<void> SetTimes(const FileTimes& times) = 0;

  /**
   * Records a delete on close request on the node.
   *
   * @param delete_on_close True to request, false to withdraw.
   */
  virtual void SetDeleteOnClose(bool delete_on_close) = 0;

  /**
   * Starts a walk of the children of this directory.
   *
   * @param pattern The wildcard pattern, empty for every child.
   * @return The walk, or kNotDirectory for a file.
   */
  [[nodiscard]] virtual Result<std::unique_ptr<IDirectory>> List(std::string_view pattern) = 0;

  /**
   * Returns the whole file as read only bytes without a copy.
   *
   * @return The bytes, valid while this file lives, or kUnsupported.
   */
  [[nodiscard]] virtual Result<std::span<const u8>> View() = 0;
};

}  // namespace rex::filesystem
