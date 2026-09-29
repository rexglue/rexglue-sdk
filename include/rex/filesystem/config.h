/**
 * @file        filesystem/config.h
 * @brief       Filesystem module slice of rex::Config
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

namespace rex::filesystem {

/** Filesystem module slice. */
struct FilesystemConfig {
  /** Allow the title to write under game_data_root. Becomes a mount flag in sub-project 3. */
  bool game_relative_writes = false;
};

}  // namespace rex::filesystem
