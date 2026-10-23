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
#include <string>
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
 * Reports whether the application runs from a package that cannot be written, an AppImage or a
 * macOS app bundle.
 *
 * @return True for an AppImage run or a macOS app bundle, false on Windows and for a loose build.
 */
[[nodiscard]] REX_API bool IsPackagedApp();

/**
 * Returns the host folder for per user configuration of an application.
 *
 * @param app_name The application name, made safe for a folder name by SanitizeFolderName.
 * @return The name joined onto the roaming application data folder on Windows, Application Support
 *         on macOS, or XDG_CONFIG_HOME falling back to .config in the home folder elsewhere. Empty
 *         when the host reports no folder or nothing is left of the name.
 */
[[nodiscard]] REX_API std::filesystem::path GetUserConfigFolder(std::string_view app_name);

/**
 * Returns the folder an application keeps its own writable files in, such as logs.
 *
 * @param app_name The application name, used when the executable folder cannot be used.
 * @return The executable folder, or GetUserConfigFolder when IsPackagedApp is true or the
 *         executable folder is not writable. Empty when that fallback gives no folder. Creates
 *         nothing.
 */
[[nodiscard]] REX_API std::filesystem::path GetAppRoot(std::string_view app_name);

/**
 * Reports whether a file can be created in a folder.
 *
 * @param dir The host folder.
 * @return True when a temporary file was created and removed, false for a missing folder, a path
 * that is not a folder, or a folder the process cannot write to.
 */
[[nodiscard]] REX_API bool IsDirectoryWritable(const std::filesystem::path& dir);

/**
 * Turns a name into a folder name that is safe on every host. Removes control characters, path
 * separators, colons, angle brackets, quotes, pipes, question marks and asterisks, trims spaces and
 * dots from both ends, and appends an underscore to the base name of a Windows device name such as
 * CON or COM1.
 *
 * @param name The raw name.
 * @param fallback The result when nothing is left of the name.
 * @return The safe name, or the fallback.
 */
[[nodiscard]] REX_API std::string SanitizeFolderName(std::string_view name,
                                                     std::string_view fallback = "default");

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
 * Joins a name onto a folder, using the name as the folder stores it when an entry matches without
 * regard to case. A name that matches no entry joins unchanged, so a caller can create it.
 *
 * @param parent The host folder to search.
 * @param name The entry name, a single path component.
 * @return The path of the existing entry whose name equals name, preferring an exact match, or
 *         parent joined with name when none exists.
 */
[[nodiscard]] REX_API std::filesystem::path ResolveEntryCase(const std::filesystem::path& parent,
                                                             std::string_view name);

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
inline u64 GetStorageFileSize(const std::filesystem::path& path) {
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
inline size_t ReadStorageFile(std::fstream& file, void* data, size_t size) {
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
inline void AppendStorageFile(std::fstream& file, const void* data, size_t size) {
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
