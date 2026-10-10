/**
 * @file        tests/support/temp_folder.h
 * @brief       Host folders that tests create and remove without throwing
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdio>
#include <exception>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

#include <rex/types.h>

namespace rex::test {

/**
 * Removes a folder and its contents. A failure prints to stderr instead of throwing.
 *
 * @param path The folder.
 */
inline void RemoveTree(const std::filesystem::path& path) noexcept {
  try {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "temp folder cleanup failed, %s\n", error.what());
  }
}

/** Uniquely named host folder under the temp directory, removed with its contents on destruction.
 */
struct TempFolder : NonCopyable {
  /**
   * Creates the folder.
   *
   * @param prefix Start of the folder name.
   */
  explicit TempFolder(std::string_view prefix = "rex_test_")
      : path(std::filesystem::temp_directory_path() /
             (std::string(prefix) + std::to_string(static_cast<u32>(std::random_device{}())))) {
    std::filesystem::create_directories(path);
  }
  ~TempFolder() { RemoveTree(path); }

  std::filesystem::path path; /**< The folder. */
};

}  // namespace rex::test
