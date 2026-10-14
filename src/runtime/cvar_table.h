/**
 * @file        runtime/cvar_table.h
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

REXCVAR_BOOL(runtime, writable_code_segments, false,
             "Enables a program to write to its own code segments in memory")
    .SetLifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_BOOL(runtime, protect_zero, true, "Protect the zero page from reads and writes")
    .SetLifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_BOOL(runtime, protect_on_release, false, "Protect released memory to prevent accesses");
REXCVAR_BOOL(runtime, scribble_heap, false, "Scribble 0xCD into all allocated heap memory");
REXCVAR_BOOL(runtime, ignore_thread_priorities, true, "Ignores game-specified thread priorities");
REXCVAR_BOOL(runtime, ignore_thread_affinities, true, "Ignores game-specified thread affinities");
REXCVAR_BOOL(runtime, ffmpeg_verbose, false, "Verbose FFmpeg output (debug and above)");
REXCVAR_TABLE(kRuntimeCvars, writable_code_segments, protect_zero, protect_on_release,
              scribble_heap, ignore_thread_priorities, ignore_thread_affinities, ffmpeg_verbose);
REXCVAR_INT32(apu, max_queued_frames, 8,
              "Audio frames queued per render client. Lower reduces latency, higher absorbs "
              "stalls")
    .SetRange(4, 64)
    .SetLifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_TABLE(kAPUCvars, max_queued_frames);

}  // namespace rex::runtime::cvars
