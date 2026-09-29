/**
 * @file        audio/cvar_table.h
 * @brief       audio module cvars
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/cvar.h>

namespace rex::audio::cvars {

REXCVAR_INT32(
    audio, max_queued_frames, 8,
    "Max buffered audio frames (range 4-64). Lower reduces latency but may cause stuttering.");
REXCVAR_BOOL(audio, mute, false, "Mute audio output");
REXCVAR_BOOL(audio, ffmpeg_verbose, false, "Verbose FFmpeg output (debug and above)");
REXCVAR_TABLE(kAudioCvars, max_queued_frames, mute, ffmpeg_verbose);

}  // namespace rex::audio::cvars
