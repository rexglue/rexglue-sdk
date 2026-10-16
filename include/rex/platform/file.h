/**
 * @file        platform/file.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::platform {

/**
 * Returns the path of the running executable.
 *
 * @return The absolute path, or an empty path when the host does not report it.
 */
REX_API std::filesystem::path GetExecutablePath();

/**
 * Returns the folder that holds the running executable.
 *
 * @return The parent of GetExecutablePath.
 */
REX_API std::filesystem::path GetExecutableFolder();

/**
 * Returns the host folder for per user data.
 *
 * @return Documents on Windows, the XDG data home on Linux and macOS.
 */
REX_API std::filesystem::path GetUserFolder();

/**
 * Reads a host file into memory.
 *
 * @param path The host file.
 * @param offset The first byte to read. An offset at the end of the file gives an empty vector.
 * @param length The number of bytes to read, 0 for all. A length past the end is a short read.
 * @return The bytes, or an IO error when the file cannot be read or the offset is past the end.
 */
[[nodiscard]] REX_API Result<std::vector<u8>> ReadHostFile(const std::filesystem::path& path,
                                                           u64 offset = 0, u64 length = 0);

/**
 * Replaces a host file with new contents. Writes a sibling temporary file and renames it over
 * the target, so readers never see a partial file. Creates missing parent folders.
 *
 * @param path The host file.
 * @param data The new contents.
 * @return An IO error when a folder, the temporary file or the rename fails.
 */
[[nodiscard]] REX_API Result<void> WriteHostFile(const std::filesystem::path& path,
                                                 std::span<const u8> data);

/**
 * Replaces a host file with text, as the span form does.
 *
 * @param path The host file.
 * @param text The new contents.
 * @return As the span form.
 */
[[nodiscard]] REX_API Result<void> WriteHostFile(const std::filesystem::path& path,
                                                 std::string_view text);

/**
 * Opens a storage file for reading and writing, creating it when missing.
 *
 * @param file Stream to open.
 * @param path Host path of the storage file.
 * @return True when the stream is open.
 */
inline bool OpenStorageFile(std::fstream& file, const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) {
    std::ofstream create(path, std::ios::binary);
  }
  file.open(path, std::ios::in | std::ios::out | std::ios::binary);
  return file.is_open();
}

/**
 * Returns the size of a storage file, or 0 when the size cannot be queried.
 *
 * @param path Host path of the storage file.
 * @return Size in bytes.
 */
inline u64 StorageFileSize(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<u64>(size);
}

/**
 * Reads bytes at the current read position.
 *
 * @param file Open storage stream.
 * @param data Destination buffer.
 * @param size Number of bytes to read.
 * @return Number of bytes read.
 */
inline size_t ReadStorage(std::fstream& file, void* data, size_t size) {
  file.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
  return static_cast<size_t>(file.gcount());
}

/**
 * Appends bytes at the end of a storage file.
 *
 * @param file Open storage stream.
 * @param data Source buffer.
 * @param size Number of bytes to write.
 */
inline void AppendStorage(std::fstream& file, const void* data, size_t size) {
  file.seekp(0, std::ios::end);
  file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

/**
 * Truncates a storage file and reopens it positioned at the end.
 *
 * @param file Storage stream, closed and reopened in place.
 * @param path Host path of the storage file.
 * @param length New length in bytes.
 */
inline void ResetStorageFile(std::fstream& file, const std::filesystem::path& path, u64 length) {
  file.close();
  std::error_code error;
  std::filesystem::resize_file(path, length, error);
  file.open(path, std::ios::in | std::ios::out | std::ios::binary);
  file.seekp(0, std::ios::end);
}

}  // namespace rex::platform
