/**
 * @file        ui/sdl3/cvar_table.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string_view>

#include <rex/cvar.h>

namespace rex::ui {

/** Console command, echoes the arguments to the log. */
void ConsoleEcho(std::string_view args);
/** Console command, lists cvar and command names containing a substring. */
void ConsoleFind(std::string_view args);

}  // namespace rex::ui

namespace rex::ui::cvars {

// NOLINTBEGIN(bugprone-throwing-static-initialization) constant descriptors registered at Connect

REXCVAR_COMMAND_ARGS(console, echo, ConsoleEcho, "Echo arguments to the console");
REXCVAR_COMMAND_ARGS(console, find, ConsoleFind, "List cvar/command names containing a substring");
REXCVAR_TABLE(kConsoleCvars, echo, find);

REXCVAR_INT32(ui, window_width, 0, "Window width in logical pixels (0 = use app default)")
    .range(0, 8192);
REXCVAR_INT32(ui, window_height, 0, "Window height in logical pixels (0 = use app default)")
    .range(0, 8192);
REXCVAR_BOOL(ui, fullscreen, true, "Start the window in fullscreen mode");
REXCVAR_BOOL(ui, fullscreen_exclusive, false,
             "Switch the display mode in fullscreen instead of going borderless at the desktop "
             "mode");
REXCVAR_INT32(ui, monitor, 0,
              "Monitor index to display on (0 = default, 1 = primary, 2 = second monitor, etc.)")
    .range(0, 16);
REXCVAR_INT32(ui, video_mode_width, 1280, "title video mode width in pixels")
    .range(640, 0x0FFF)
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_INT32(ui, video_mode_height, 720, "title video mode height in pixels")
    .range(480, 0x0FFF)
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_STRING(ui, resolution, "",
               "Common resolution preset for both title video mode and window (for example: 720p, "
               "1080p, 1440p, 4k, 1280x720)");
REXCVAR_DOUBLE(ui, video_mode_refresh_rate, 60.0, "title video mode refresh rate in Hz")
    .range(24.0, 240.0)
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_DOUBLE(ui, cursor_timeout, 0.0,
               "Seconds without mouse movement over the window before the cursor hides, 0 never")
    .range(0.0, 3600.0);
REXCVAR_TABLE(kUICvars, window_width, window_height, fullscreen, fullscreen_exclusive, monitor,
              video_mode_width, video_mode_height, resolution, video_mode_refresh_rate,
              cursor_timeout);

// NOLINTEND(bugprone-throwing-static-initialization)

}  // namespace rex::ui::cvars
