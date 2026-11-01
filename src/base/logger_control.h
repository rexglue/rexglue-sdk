/**
 * @file        base/logger_control.h
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

#include <rex/module.h>
#include <rex/result.h>

namespace rex {

/** Private control of the process logger. */
REX_DECLARE_PRIVATE_INTERFACE(ILoggerControl, 1)
/** Applies the settings once the command line, environment and config file are read. */
  virtual void ApplySettled() = 0;

  /**
   * Opens the log file once the log root resolves.
   *
   * @param app_name Names the file when log.file_name is empty.
   * @return Ok, or why the file cannot open.
   */
  [[nodiscard]] virtual Result<void> OpenFile(const std::filesystem::path& log_root,
                                              std::string_view app_name) = 0;

  /** Ends the logger session so a process can run another app. */
  virtual void EndSession() = 0;
};

}  // namespace rex
