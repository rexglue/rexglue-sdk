#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/cvar.h>

// Input/HID configuration flags
REXCVAR_DECLARE(bool, guide_button);
REXCVAR_DECLARE(std::string, hid_mappings_file);
REXCVAR_DECLARE(std::string, input_backend);
REXCVAR_DECLARE(bool, vibration);
REXCVAR_DECLARE(double, left_stick_deadzone_percentage);
REXCVAR_DECLARE(double, right_stick_deadzone_percentage);
