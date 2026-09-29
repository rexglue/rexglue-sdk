/**
 * @file        ui/overlay/console_commands.cpp
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

#include "ui/cvar_table.h"

namespace rex::ui {

void ConsoleEcho(std::string_view args) {
  REXLOG_INFO("{}", args);
}

void ConsoleFind(std::string_view args) {
  while (!args.empty() && args.front() == ' ')
    args.remove_prefix(1);
  while (!args.empty() && args.back() == ' ')
    args.remove_suffix(1);
  if (args.empty()) {
    REXLOG_INFO("find: usage: find <substr>");
    return;
  }
  auto* registry = ::rex::ui::cvars::echo.registry();
  if (!registry) {
    REXLOG_INFO("find: no registry");
    return;
  }
  std::string needle(args);
  int matches = 0;
  for (const rex::cvar::IVar* var : registry->Enumerate()) {
    const std::string_view n = var->desc().name;
    if (n.find(needle) != std::string_view::npos) {
      REXLOG_INFO("  {}", n);
      ++matches;
    }
  }
  REXLOG_INFO("find: {} match(es) for '{}'", matches, needle);
}

}  // namespace rex::ui
