/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_CONSOLE_APP_MAIN_H_
#define XENIA_BASE_CONSOLE_APP_MAIN_H_

#include <string>
#include <vector>

namespace rex {

// Extern defined by user code. This must be present for the application to
// launch.
struct ConsoleAppEntryInfo {
  std::string name;
  int (*entry_point)(const std::vector<std::string>& args);
  bool transparent_options;  // no argument parsing
  std::string positional_usage;
  std::vector<std::string> positional_options;
};
ConsoleAppEntryInfo GetConsoleAppEntryInfo();

// TODO(Triang3l): Multiple console app entry points on Android when a console
// activity is added. This is currently for individual executables running on
// Termux.

#define XE_DEFINE_CONSOLE_APP(name, entry_point, positional_usage, ...)    \
  rex::ConsoleAppEntryInfo rex::GetConsoleAppEntryInfo() {                   \
    std::initializer_list<std::string> positional_options = {__VA_ARGS__}; \
    return rex::ConsoleAppEntryInfo{                                        \
        name, entry_point, false, positional_usage,                        \
        std::vector<std::string>(std::move(positional_options))};          \
  }

// TODO(Joel Linn): Add some way to filter consumed arguments in
// cvar::ParseLaunchArguments()
#define XE_DEFINE_CONSOLE_APP_TRANSPARENT(name, entry_point)               \
  rex::ConsoleAppEntryInfo rex::GetConsoleAppEntryInfo() {                   \
    return rex::ConsoleAppEntryInfo{name, entry_point, true, std::string(), \
                                   std::vector<std::string>()};            \
  }

}  // namespace rex

#endif  // XENIA_BASE_CONSOLE_APP_MAIN_H_
