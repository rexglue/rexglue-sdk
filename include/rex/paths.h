/**
 * @file        paths.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <string_view>

#include <rex/export.h>
#include <rex/result.h>

namespace rex {

/**
 * Host paths the SDK works with. An empty field keeps the behavior documented on it, and
 * ResolvePaths fills the ones it documents.
 */
struct PathsConfig {
  /** Folder holding default.xex. TitleApp::LaunchTitle fails when it is empty. */
  std::filesystem::path game_data_root;
  /**
   * Saves and profiles. Empty means game_data_root. ResolvePaths fills the platform user folder
   * plus the application name.
   */
  std::filesystem::path user_data_root;
  /** Title update packages. Empty means none. */
  std::filesystem::path update_data_root;
  /**
   * Shader storage and other caches. Empty means none. ResolvePaths fills user_data_root plus
   * "cache".
   */
  std::filesystem::path cache_root;
  /** Optional metadata. Empty means none. */
  std::filesystem::path metadata_root;
  /**
   * The TOML file for LoadConfigFile and SaveConfigFile. Empty means none. ResolvePaths fills
   * platform::GetAppRoot plus the application name and ".toml".
   */
  std::filesystem::path config_file;
};

/**
 * Fills the empty user_data_root, cache_root and config_file of a PathsConfig. Fields that are set,
 * and the other fields, are copied unchanged. Creates the application root folder when it fills
 * config_file.
 *
 * @param paths The paths to start from.
 * @param app_name The application name, usually the executable name without its extension.
 * @return The filled paths, or a config error when the name is empty or the host gives no folder
 *         for a field that needs one.
 */
[[nodiscard]] REX_API Result<PathsConfig> ResolvePaths(const PathsConfig& paths,
                                                       std::string_view app_name);

}  // namespace rex
