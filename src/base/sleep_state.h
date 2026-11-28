/**
 * @file        base/sleep_state.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>
#include <rex/types.h>

namespace rex::platform::detail {

/** Counts the sleeps that used ::Sleep or SleepEx because the high resolution timer failed. */
REX_API u64 SleepTimerFallbackCount();

}  // namespace rex::platform::detail
