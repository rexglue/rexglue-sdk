/**
 * @file        cvar_file.h
 * @brief       How flags and the settings file meet: app defaults, file values
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

// What SaveConfig (rex/cvar.h) writes: every flag that differs from its
// default, except that a value given on the command line or in the
// environment is a one-off for that run, so the file keeps its own line for
// that flag (or none). A change made while the app runs (SetFlagByName) is
// saved as usual, over a command-line value too. Lines of the file for flags
// nobody registered in this run (a plugin not loaded, another program's
// settings) are written back as they were read instead of being dropped.
//
// Kept out of rex/cvar.h so that adding these doesn't change a header every
// recompiled game file includes (through rex/chrono/clock.h).

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace rex::cvar {

/// Make `value` the default of a registered flag: the value it has while
/// nothing else set it, the value ResetToDefault goes back to, and the value
/// the settings file leaves out. For an app whose defaults differ from the
/// SDK's: the player can then save the SDK's own value (it gets a line in
/// the file) and the app's default doesn't come back at the next start. The
/// current value changes too (change callbacks run) while the flag is still at
/// its default.
/// @return false for an unknown flag or a value it doesn't take
bool SetDefaultValue(std::string_view name, std::string_view value);

/// The value the settings file has for this flag (as of the last LoadConfig
/// or SaveConfig), even when a higher-priority source won over it this run;
/// nullopt when the file has no line for it.
std::optional<std::string> GetConfigFileValue(std::string_view name);

}  // namespace rex::cvar
