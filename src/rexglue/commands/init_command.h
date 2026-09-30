/**
 * @file        rexglue/commands/init_command.h
 * @brief       Project initialization command interface
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#pragma once

#include "../cli_utils.h"

#include <cstdint>
#include <string>

#include <rex/result.h>

namespace CLI {
class App;
}

namespace rexglue::cli {

using rex::Result;

struct InitOptions {
  std::string project_name;
  std::string xex_path;
  std::string game_root;
  std::string project_root;
  bool scan_dlls = false;
  std::string template_dir;
  bool force = false;
};

struct InitModuleOptions {
  std::string app_root;
  std::string xex_path;
  std::string guest_path;
};

struct InitAchievementsOptions {
  std::string xex_path;
  std::string output_dir;
  // XLanguage ID to extract strings for; defaults to English (1) when unset.
  uint32_t language = 1;
};

Result<void> InitProject(const InitOptions& opts, const CLIContext& ctx);
Result<void> InitModule(const InitModuleOptions& opts, const CLIContext& ctx);
Result<void> InitAchievements(const InitAchievementsOptions& opts, const CLIContext& ctx);

void RegisterInit(CLI::App& parent, const CLIContext& ctx, DeferredAction& pending);

}  // namespace rexglue::cli
