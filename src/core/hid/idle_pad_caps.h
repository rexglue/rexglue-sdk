/**
 * @file        core/hid/idle_pad_caps.h
 * @brief       Capabilities of the idle pad
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/export.h>
#include <rex/hid/xinput.h>

namespace rex::hid::detail {

/**
 * Builds the capabilities every idle pad reports.
 *
 * @return A gamepad with every button, both triggers, both sticks and both motors.
 */
REX_API X_INPUT_CAPABILITIES MakeIdlePadCapabilities();

}  // namespace rex::hid::detail
