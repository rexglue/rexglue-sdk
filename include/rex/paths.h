/**
 * @file        paths.h
 * @brief       Host path configuration
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>

namespace rex {

/**
 * Host paths the SDK works with. Empty fields keep the documented default.
 */
struct PathsConfig {
  /** Folder holding default.xex. REQUIRED before the Runtime is constructed. */
  std::filesystem::path game_data_root;
  /** Saves and profiles. Empty means the platform user folder plus the application name. */
  std::filesystem::path user_data_root;
  /** Title update packages. Empty means none. */
  std::filesystem::path update_data_root;
  /** Shader storage and other caches. Empty means user_data_root / "cache". */
  std::filesystem::path cache_root;
  /** Optional metadata. Empty means none. */
  std::filesystem::path metadata_root;
  /**
   * The TOML file for LoadConfigFile and SaveConfigFile. Empty means the executable folder plus
   * the application name and ".toml".
   */
  std::filesystem::path config_file;
};

}  // namespace rex
