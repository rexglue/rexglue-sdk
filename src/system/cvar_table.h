/**
 * @file        system/cvar_table.h
 * @brief       system module cvars
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/cvar.h>

namespace rex::runtime::cvars {

REXCVAR_BOOL(runtime, pix, false, "Enable PIX debugging support");
REXCVAR_BOOL(runtime, log_high_frequency_kernel_calls, false,
             "Log kernel calls with the kHighFrequency tag");
REXCVAR_BOOL(runtime, debug_monitor, false, "Enable debug monitor");
REXCVAR_BOOL(runtime, cert_monitor, false, "Enable cert monitor");
REXCVAR_BOOL(runtime, writable_code_segments, false,
             "Enables a program to write to its own code segments in memory")
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_BOOL(runtime, protect_zero, true, "Protect the zero page from reads and writes")
    .lifecycle(rex::cvar::Lifecycle::RequiresRestart);
REXCVAR_BOOL(runtime, protect_on_release, false, "Protect released memory to prevent accesses");
REXCVAR_BOOL(runtime, scribble_heap, false, "Scribble 0xCD into all allocated heap memory");
REXCVAR_BOOL(runtime, ignore_thread_priorities, true, "Ignores game-specified thread priorities");
REXCVAR_BOOL(runtime, ignore_thread_affinities, true, "Ignores game-specified thread affinities");
REXCVAR_TABLE(kRuntimeCvars, pix, log_high_frequency_kernel_calls, debug_monitor, cert_monitor,
              writable_code_segments, protect_zero, protect_on_release, scribble_heap,
              ignore_thread_priorities, ignore_thread_affinities);

}  // namespace rex::runtime::cvars
