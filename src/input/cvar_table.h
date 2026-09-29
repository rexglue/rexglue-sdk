/**
 * @file        input/cvar_table.h
 * @brief       input module cvars
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/cvar.h>

namespace rex::input::cvars {

REXCVAR_BOOL(input, guide_button, false, "Enable guide button pass-through");
REXCVAR_BOOL(input, vibration, true, "Enable controller vibration");
REXCVAR_DOUBLE(input, left_stick_deadzone, 0.0,
               "Deadzone applied to the left stick, as a fraction of its range")
    .range(0.0, 1.0);
REXCVAR_DOUBLE(input, right_stick_deadzone, 0.0,
               "Deadzone applied to the right stick, as a fraction of its range")
    .range(0.0, 1.0);
REXCVAR_BOOL(input, mnk_mode, false, "Enable keyboard/mouse controller emulation");
REXCVAR_BOOL(input, mnk_mouse, false,
             "Use the mouse for the right stick. Off means the right stick comes from the "
             "keybind_rstick_* keys only");
REXCVAR_DOUBLE(input, mnk_sensitivity, 1.0, "Mouse sensitivity for right stick").range(0.01, 10.0);
REXCVAR_BOOL(input, mnk_passthrough, false,
             "Surface raw keystrokes to the title instead of emulating a pad, for titles that read "
             "a USB keyboard. Enables the device on its own");
REXCVAR_STRING(input, keybind_a, "Semicolon,Space", "A button");
REXCVAR_STRING(input, keybind_b, "Quote,Backspace", "B button");
REXCVAR_STRING(input, keybind_x, "L", "X button");
REXCVAR_STRING(input, keybind_y, "P", "Y button");
REXCVAR_STRING(input, keybind_left_trigger, "Q,I", "Left trigger");
REXCVAR_STRING(input, keybind_right_trigger, "E,O", "Right trigger");
REXCVAR_STRING(input, keybind_left_shoulder, "1", "Left shoulder");
REXCVAR_STRING(input, keybind_right_shoulder, "3", "Right shoulder");
REXCVAR_STRING(input, keybind_lstick_up, "W", "Left stick up");
REXCVAR_STRING(input, keybind_lstick_down, "S", "Left stick down");
REXCVAR_STRING(input, keybind_lstick_left, "A", "Left stick left");
REXCVAR_STRING(input, keybind_lstick_right, "D", "Left stick right");
REXCVAR_STRING(input, keybind_lstick_press, "F", "Left stick press");
REXCVAR_STRING(input, keybind_rstick_up, "Up", "Right stick up");
REXCVAR_STRING(input, keybind_rstick_down, "Down", "Right stick down");
REXCVAR_STRING(input, keybind_rstick_left, "Left", "Right stick left");
REXCVAR_STRING(input, keybind_rstick_right, "Right", "Right stick right");
REXCVAR_STRING(input, keybind_rstick_press, "K", "Right stick press");
REXCVAR_STRING(input, keybind_dpad_up, "Shift+Up", "D-pad up");
REXCVAR_STRING(input, keybind_dpad_down, "Shift+Down", "D-pad down");
REXCVAR_STRING(input, keybind_dpad_left, "Shift+Left", "D-pad left");
REXCVAR_STRING(input, keybind_dpad_right, "Shift+Right", "D-pad right");
REXCVAR_STRING(input, keybind_back, "Z,Tab", "Back button");
REXCVAR_STRING(input, keybind_start, "X,Return", "Start button");
REXCVAR_STRING(input, keybind_guide, "", "Guide button");
REXCVAR_TABLE(kInputCvars, guide_button, vibration, left_stick_deadzone, right_stick_deadzone,
              mnk_mode, mnk_mouse, mnk_sensitivity, mnk_passthrough, keybind_a, keybind_b,
              keybind_x, keybind_y, keybind_left_trigger, keybind_right_trigger,
              keybind_left_shoulder, keybind_right_shoulder, keybind_lstick_up, keybind_lstick_down,
              keybind_lstick_left, keybind_lstick_right, keybind_lstick_press, keybind_rstick_up,
              keybind_rstick_down, keybind_rstick_left, keybind_rstick_right, keybind_rstick_press,
              keybind_dpad_up, keybind_dpad_down, keybind_dpad_left, keybind_dpad_right,
              keybind_back, keybind_start, keybind_guide);

}  // namespace rex::input::cvars
