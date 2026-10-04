/**
 * @file        logging/config.h
 * @brief       Logger config inside rex::Config
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <rex/logging/format.h>
#include <rex/logging/level.h>
#include <rex/types.h>

namespace rex {

namespace log {
class Category;
class ISink;
}  // namespace log

/** Log file policy. */
struct LogFileConfig {
  /** Opens a file. App sets it to true before OnConfigure. */
  bool enabled = false;
  /** Explicit file. Empty names a sequential file in folder. */
  std::filesystem::path path;
  /** Folder of sequential files. Empty means the logs folder next to the executable. */
  std::filesystem::path folder;
  /** Base name of sequential files. Empty means the executable name. */
  std::string name;
  /** Bytes of older runs kept in folder, 0 for no limit. */
  u64 folder_budget = 0;
  /** Line format of the file. */
  log::LineFormat format{log::TimestampStyle::kDateTime, true, true, false};
};

/** Logger configuration, read once when the modules connect. */
struct LogConfig {
  /** Default level. */
  log::Level level = log::Level::kInfo;
  /** Levels by category name, applied when the category registers. */
  std::map<std::string, log::Level, std::less<>> levels;
  /** Consumer categories, registered before any module connects. */
  std::vector<const log::Category*> categories;
  /** Format of the console and debugger sinks. */
  log::LineFormat format;
  /** Writes to standard output. For console apps and tools. */
  bool console = false;
  /** Writes to the debugger. */
  bool debugger = true;
  /** Log file policy. */
  LogFileConfig file;
  /** Consumer sinks, added at start and removed at stop. Not owned. */
  std::vector<log::ISink*> sinks;
  /** Records at or above it flush every sink. */
  log::Level flush_level = log::Level::kWarn;
  /** Period of the flush thread, 0 for none. */
  std::chrono::seconds flush_interval{1};
};

}  // namespace rex
