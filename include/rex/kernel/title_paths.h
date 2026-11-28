/**
 * @file        kernel/title_paths.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <string>

#include <rex/platform.h>
#include <rex/setting.h>

namespace rex::kernel {

class REX_KERNEL_API TitlePaths final : public SettingOwner {
 public:
  REX_SETTING(std::filesystem::path, game_root, "",
              "Game files, mounted as game:, read only unless game_writable is set", kLockedInGame);
  REX_SETTING(bool, game_writable, false, "Mount game: read write instead of read only",
              kLockedInGame);
  REX_SETTING(std::filesystem::path, update_root, "", "Title update files, empty for none",
              kLockedInGame);
  REX_SETTING(std::filesystem::path, metadata_root, "",
              "Achievement icons and other title metadata, empty searches near title.game_root",
              kLockedInGame);
  REX_SETTING(std::string, entry, "default.xex",
              "Executable under game: that TitleRuntime::Load loads, empty loads none",
              kLockedInGame);
  REX_SETTING(std::string, imports, "",
              "Files under game: the executable may import from, separated by |", kLockedInGame);

  TitlePaths();
};

}  // namespace rex::kernel
