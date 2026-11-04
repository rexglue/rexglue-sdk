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
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::platform {

/**
 * @return The absolute path, or empty when the host does not report it.
 */
REX_API std::filesystem::path GetExecutablePath();

/**
 * @return The parent of GetExecutablePath.
 */
REX_API std::filesystem::path GetExecutableFolder();

/**
 * @return Documents on Windows, the XDG data home on Linux and macOS.
 */
REX_API std::filesystem::path GetUserFolder();

/**
 * Reports whether the application runs from a read only package.
 *
 * @return True for an AppImage or a macOS app bundle, false on Windows and for a loose build.
 */
[[nodiscard]] REX_API bool IsPackagedApp();

/**
 * Returns the host folder for per user configuration of an application. It is under roaming
 * application data on Windows, Application Support on macOS, or XDG_CONFIG_HOME elsewhere with
 * .config as the fallback. It is empty when no folder is found.
 *
 * @param app_name The application name, passed through SanitizeFolderName.
 */
[[nodiscard]] REX_API std::filesystem::path GetUserConfigFolder(std::string_view app_name);

/**
 * Returns the folder an application keeps its writable files in, such as logs. Creates nothing. It
 * is the executable folder, or GetUserConfigFolder when IsPackagedApp is true or the executable
 * folder is not writable. It is empty when neither folder is usable.
 *
 * @param app_name The application name, used for the fallback.
 */
[[nodiscard]] REX_API std::filesystem::path GetAppRoot(std::string_view app_name);

/**
 * Reports whether a file can be created in a folder.
 *
 * @return True when a temporary file could be created there.
 */
[[nodiscard]] REX_API bool IsDirectoryWritable(const std::filesystem::path& dir);

/**
 * Turns a name into a folder name that is safe on every host. Removes control characters and
 * reserved punctuation, trims spaces and dots, and appends an underscore to Windows device names
 * such as CON or COM1.
 *
 * @param fallback The result when nothing is left of the name.
 * @return The safe name, or the fallback.
 */
[[nodiscard]] REX_API std::string SanitizeFolderName(std::string_view name,
                                                     std::string_view fallback = "default");

/**
 * Reads a host file into memory.
 *
 * @param offset The first byte to read. An offset at the end gives an empty vector.
 * @param length The byte count, 0 for all. A length past the end is a short read.
 * @return The bytes, or an IO error when unreadable or the offset is past the end.
 */
[[nodiscard]] REX_API Result<std::vector<u8>> ReadHostFile(const std::filesystem::path& path,
                                                           u64 offset = 0, u64 length = 0);

/**
 * Replaces a host file atomically through a temporary file and a rename. Creates missing parent
 * folders.
 *
 * @return An IO error when a folder, the temporary file or the rename fails.
 */
[[nodiscard]] REX_API Result<void> WriteHostFile(const std::filesystem::path& path,
                                                 std::span<const u8> data);

/** Replaces a host file with text, as the span form does. */
[[nodiscard]] REX_API Result<void> WriteHostFile(const std::filesystem::path& path,
                                                 std::string_view text);

/**
 * Joins a name onto a folder, keeping the case of an existing entry that matches ignoring case.
 *
 * @param name The entry name, a single path component.
 * @return The existing entry path, preferring an exact match, or parent joined with name.
 */
[[nodiscard]] REX_API std::filesystem::path ResolveEntryCase(const std::filesystem::path& parent,
                                                             std::string_view name);

/** Opens a storage file for reading and writing, creating it when missing. */
REX_API bool OpenStorageFile(std::fstream& file, const std::filesystem::path& path);

/**
 * @return Size in bytes, 0 when the size cannot be queried.
 */
inline u64 GetStorageFileSize(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<u64>(size);
}

/**
 * Reads bytes at the current read position.
 *
 * @return Number of bytes read.
 */
REX_API size_t ReadStorageFile(std::fstream& file, void* data, size_t size);

REX_API void AppendStorageFile(std::fstream& file, const void* data, size_t size);

/**
 * Truncates a storage file and reopens it positioned at the end.
 *
 * @param file Storage stream, closed and reopened in place.
 */
REX_API void ResetStorageFile(std::fstream& file, const std::filesystem::path& path, u64 length);

}  // namespace rex::platform
