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

#pragma once

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, headless);
REXCVAR_DECLARE(bool, log_high_frequency_kernel_calls);
REXCVAR_DECLARE(bool, xex_apply_patches);
REXCVAR_DECLARE(uint32_t, license_mask);
REXCVAR_DECLARE(uint32_t, user_country);
REXCVAR_DECLARE(uint32_t, user_language);
REXCVAR_DECLARE(bool, kernel_pix);
REXCVAR_DECLARE(std::string, cl);
REXCVAR_DECLARE(bool, kernel_debug_monitor);
REXCVAR_DECLARE(bool, kernel_cert_monitor);
REXCVAR_DECLARE(bool, ignore_thread_priorities);
REXCVAR_DECLARE(bool, ignore_thread_affinities);
REXCVAR_DECLARE(bool, writable_executable_memory);
REXCVAR_DECLARE(bool, protect_zero);
REXCVAR_DECLARE(bool, protect_on_release);
REXCVAR_DECLARE(bool, scribble_heap);
