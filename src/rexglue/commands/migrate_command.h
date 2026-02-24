/**
 * @file        rexglue/commands/migrate_command.h
 * @brief       Project migration command
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#pragma once

#include <string>

#include <rex/result.h>

#include "../cli_utils.h"

namespace rexglue::cli {

struct MigrateOptions {
  std::string app_root;
  bool force = false;  // Skip confirmation prompt
};

/**
 * Migrate an existing rexglue project to the current SDK version.
 * Overwrites src/main.cpp and CMakeLists.txt with the latest templates.
 */
rex::Result<void> MigrateProject(const MigrateOptions& opts, const CliContext& ctx);

}  // namespace rexglue::cli
