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
              "Game files, mounted as game:, read only unless game_writable is set", kUntilLaunch);
  REX_SETTING(bool, game_writable, false, "Mount game: read write instead of read only",
              kUntilLaunch);
  REX_SETTING(std::filesystem::path, update_root, "", "Title update files, empty for none",
              kUntilLaunch);
  REX_SETTING(std::filesystem::path, metadata_root, "{app.root}/metadata",
              "Achievement icons and other title metadata", kUntilLaunch);
  REX_SETTING(std::string, entry, "default.xex",
              "Executable under game: that TitleRuntime::Load runs, empty loads none",
              kUntilLaunch);
  REX_SETTING(std::string, imports, "",
              "Files under game: the executable may import from, separated by |", kUntilLaunch);

  TitlePaths();
};

}  // namespace rex::kernel
