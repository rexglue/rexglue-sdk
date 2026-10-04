/**
 * @file        core/cvar_table.h
 * @brief       core module cvars
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/cvar.h>

namespace rex::cvars {

// NOLINTBEGIN(bugprone-throwing-static-initialization) constant descriptors registered at Connect

REXCVAR_BOOL(clock, no_scaling, false, "Disable clock scaling (inverted: false = scaling enabled)");
REXCVAR_BOOL(clock, source_raw, false, "Use raw clock source without scaling");
REXCVAR_TABLE(kClockCvars, no_scaling, source_raw);

REXCVAR_STRING(log, level, "info", "Default log level")
    .allowed(REXCVAR_ALLOWED("trace", "dev", "debug", "info", "warn", "error", "critical", "off"));
REXCVAR_STRING(log, file, "", "Log file path, empty for a numbered file in the logs folder")
    .lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_INT32(log, flush_interval, 1, "Seconds between log flushes, 0 for none")
    .range(0, 60)
    .lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_TABLE(kLogCvars, level, file, flush_interval);

REXCVAR_BOOL(perf, profiler, false, "Start the Tracy profiler at startup when the build has it")
    .lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_TABLE(kPerfCvars, profiler);

// NOLINTEND(bugprone-throwing-static-initialization)

}  // namespace rex::cvars
