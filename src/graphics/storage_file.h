/**
 * @file        graphics/storage_file.h
 * @brief       Host stream helpers for the persistent shader and pipeline storage
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
#include <system_error>

#include <rex/types.h>

namespace rex::graphics {

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
 * Returns the size of a storage file, or 0 when the size is unknown.
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

}  // namespace rex::graphics
