/**
 * @file        core/cvar_table.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/cvar.h>
#include <rex/logging/level.h>

namespace rex::cvars {

REXCVAR_BOOL(clock, no_scaling, false, "Disable clock scaling (inverted: false = scaling enabled)");
REXCVAR_TABLE(kClockCvars, no_scaling);

REXCVAR_STRING(log, level, "info", "Default log level").SetAllowed(rex::log::kLevelNames);
REXCVAR_STRING(log, file, "", "Log file path, empty for a numbered file in the logs folder")
    .SetLifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_INT32(log, flush_interval, 1, "Seconds between log flushes, 0 for none")
    .SetRange(0, 60)
    .SetLifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_TABLE(kLogCvars, level, file, flush_interval);

REXCVAR_BOOL(perf, profiler, false, "Start the Tracy profiler at startup when the build has it")
    .SetLifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_TABLE(kPerfCvars, profiler);

}  // namespace rex::cvars
