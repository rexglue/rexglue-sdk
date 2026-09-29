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

REXCVAR_BOOL(clock, no_scaling, false, "Disable clock scaling (inverted: false = scaling enabled)");
REXCVAR_BOOL(clock, source_raw, false, "Use raw clock source without scaling");
REXCVAR_TABLE(kClockCvars, no_scaling, source_raw);

REXCVAR_STRING(log, level, "info",
               "Global log level: trace, debug, info, warn, error, critical, off")
    .allowed(REXCVAR_ALLOWED("trace", "debug", "info", "warn", "error", "critical", "off"));
REXCVAR_STRING(log, file, "", "Log file path (empty = auto sequential naming)");
REXCVAR_BOOL(log, verbose, false, "Enable verbose logging (sets level to trace)").debug_only();
REXCVAR_BOOL(log, noisy, false, "Enable noisy/high-frequency log macros");
REXCVAR_INT32(log, flush_interval, 0, "Periodic flush interval in seconds (0 = off)")
    .range(0, 60)
    .lifecycle(rex::cvar::Lifecycle::InitOnly);
REXCVAR_TABLE(kLogCvars, level, file, verbose, noisy, flush_interval);

REXCVAR_STRING(perf, log_csv, "", "Path to write per-frame CSV log (empty = disabled)");
REXCVAR_TABLE(kPerfCvars, log_csv);

}  // namespace rex::cvars
