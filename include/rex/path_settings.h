/**
 * @file        path_settings.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>

#include <rex/platform.h>
#include <rex/setting.h>

namespace rex {

/** The app.* paths that modules read, built on app.root and app.name. */
class REX_API PathSettings final : public SettingOwner {
 public:
  PathSettings();

  REX_SETTING(std::filesystem::path, config_file, "{app.root}/{app.name}.toml", "Config file",
              kStartupOnly);
  REX_SETTING(std::filesystem::path, user_root, "{app.root}/user", "Saves and per player data",
              kStartupOnly);
  REX_SETTING(std::filesystem::path, cache_root, "",
              "Shader and pipeline caches, no shader storage when empty", kStartupOnly);
};

/** Returns the path settings. The first call registers them. */
REX_API PathSettings& GetPathSettings();

#define REX_PATHS() ::rex::GetPathSettings()

}  // namespace rex
