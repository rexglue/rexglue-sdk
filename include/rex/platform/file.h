/**
 * @file        platform/file.h
 * @brief       Host folder queries and whole file helpers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include <rex/macros.h>
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
 * @return The file bytes, or an IO error when the file cannot be opened or read.
 */
[[nodiscard]] REX_API Result<std::vector<u8>> ReadWholeFile(const std::filesystem::path& path);

/**
 * Replaces a host file with new contents. Writes a sibling temporary file and renames it over
 * the target, so readers never see a partial file. Creates missing parent folders.
 *
 * @param path The host file.
 * @param data The new contents.
 * @return An IO error when a folder, the temporary file or the rename fails.
 */
[[nodiscard]] REX_API Result<void> WriteWholeFile(const std::filesystem::path& path,
                                                  std::span<const u8> data);

}  // namespace rex::platform
