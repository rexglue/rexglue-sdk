/**
 * @file        filesystem.h
 * @brief       The filesystem facade and the filesystem module config of rex::Config
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/filesystem/directory.h>
#include <rex/filesystem/error.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/file_system.h>
#include <rex/filesystem/info.h>
#include <rex/filesystem/mount.h>
#include <rex/filesystem/open.h>
#include <rex/paths.h>

namespace rex::filesystem {

/** Filesystem module config. */
struct FilesystemConfig {
  /** Mount flags of the game data mount. kWritable lets the title write under game_data_root. */
  MountFlags game_mount_flags = MountFlags::kNone;
};

}  // namespace rex::filesystem
