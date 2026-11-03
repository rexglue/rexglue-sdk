/**
 * @file        ui/overlay/console_command.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string_view>

#include <rex/result.h>
#include <rex/setting.h>
#include <rex/settings.h>
#include <rex/command.h>

namespace rex::ui {

/** Owns the console.echo and console.find commands. */
class ConsoleCommandTable final : public SettingOwner {
 public:
  /**
   * @param settings The registry find searches.
   */
  explicit ConsoleCommandTable(ISettings& settings);

  REX_COMMAND(echo, "Echo arguments to the console",
              [](std::string_view args) { return Echo(args); });
  REX_COMMAND(find, "List setting and command names containing a substring",
              [this](std::string_view args) { return Find(args); });

 private:
  static Result<void> Echo(std::string_view args);
  Result<void> Find(std::string_view args);

  ISettings& settings_;
};

}  // namespace rex::ui
