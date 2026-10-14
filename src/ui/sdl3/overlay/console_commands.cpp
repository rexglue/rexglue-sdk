/**
 * @file        ui/sdl3/overlay/console_commands.cpp
 *
 * @brief       Argument-taking console commands (echo, find) registered through the
 *              console cvar table.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/cvar.h>
#include <rex/logging.h>

#include <string>
#include <string_view>

#include "ui/sdl3/cvar_table.h"

namespace rex::ui {

void ConsoleEcho(std::string_view args) {
  REXLOG_INFO(console, "{}", args);
}

void ConsoleFind(std::string_view args) {
  const auto first = args.find_first_not_of(' ');
  args = first == std::string_view::npos
             ? std::string_view{}
             : args.substr(first, args.find_last_not_of(' ') - first + 1);
  if (args.empty()) {
    REXLOG_INFO(console, "find: usage: find <substr>");
    return;
  }
  auto* registry = ::rex::ui::cvars::echo.GetRegistry();
  if (!registry) {
    REXLOG_INFO(console, "find: no registry");
    return;
  }
  std::string needle(args);
  int matches = 0;
  for (const rex::cvar::IVar* var : registry->Enumerate()) {
    const std::string_view n = var->GetDesc().name;
    if (n.find(needle) != std::string_view::npos) {
      REXLOG_INFO(console, "  {}", n);
      ++matches;
    }
  }
  REXLOG_INFO(console, "find: {} match(es) for '{}'", matches, needle);
}

}  // namespace rex::ui
