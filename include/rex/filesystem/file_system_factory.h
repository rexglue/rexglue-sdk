/**
 * @file        filesystem/file_system_factory.h
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

#include <rex/filesystem/file_system.h>
#include <rex/filesystem/xdbf.h>
#include <rex/filesystem/xex.h>
#include <rex/kernel/types/xex.h>
#include <rex/module.h>
#include <rex/result.h>

namespace rex::filesystem {

class IContainerImage;
struct XContentPackageHeader;

/** Creates file systems and parsers. Its objects must be destroyed before the module unloads. */
REX_DECLARE_INTERFACE(IFileSystemFactory, 2)
/**
 * Creates an empty mount table.
 *
 * @return The file system, which must be destroyed before the module unloads.
 */
  [[nodiscard]] virtual std::unique_ptr<IFileSystem> Create() = 0;

  /**
   * Parses the header of an XEX1 or XEX2 file.
   *
   * @return The image, or a Format error for a bad or truncated header.
   */
  [[nodiscard]] virtual Result<std::unique_ptr<IXEXImage>> ParseXEX(std::span<const u8> file) = 0;

  /**
   * Maps and parses a container. A folder is searched breadth first for a package.
   *
   * @param path The container file or a folder that holds one.
   * @return The container, kNotFound when nothing opens, kCorrupt for a bad image.
   */
  [[nodiscard]] virtual Result<std::unique_ptr<IContainerImage>> OpenContainer(
      const std::filesystem::path& path) = 0;

  /**
   * Wraps an in memory XDBF database.
   *
   * @param data The database bytes, which must outlive the reader.
   * @param data_size Size in bytes.
   * @return The reader, invalid for null data.
   */
  [[nodiscard]] virtual std::unique_ptr<IXDBFTitleData> OpenXDBF(const u8* data,
                                                                 size_t data_size) = 0;

  /**
   * Parses the PE headers of a decoded image.
   *
   * @return The PE info, or a Format error for a non PowerPC PE image.
   */
  [[nodiscard]] virtual Result<PEInfo> ParsePE(std::span<const u8> image) = 0;

  /**
   * Finds an optional header entry in a raw XEX header.
   *
   * @param header The header bytes.
   * @return The entry, or nothing when absent.
   */
  [[nodiscard]] virtual std::optional<XEXOptHeaderEntry> FindXEXOptHeader(
      std::span<const u8> header, kernel::XEXHeaderKey key) = 0;

  /**
   * Reads the package header from the first bytes of a package.
   *
   * @param bytes At least sizeof(XContentPackageHeader) bytes from the package start.
   * @return The header, or kCorrupt for short bytes or a bad magic.
   */
  [[nodiscard]] virtual Result<XContentPackageHeader> ReadPackageHeader(
      std::span<const u8> bytes) = 0;

  /**
   * Reads the package header of a host file.
   *
   * @return The header, kNotFound when unopened, or kCorrupt.
   */
  [[nodiscard]] virtual Result<XContentPackageHeader> ReadPackageHeader(
      const std::filesystem::path& path) = 0;
};

}  // namespace rex::filesystem
