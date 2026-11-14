/**
 * @file        base/timer_queue_state.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>
#include <rex/platform/timer.h>

namespace rex::platform::detail {

/** Reports whether the queue exists, from the first timer until ShutdownTimerQueue. */
REX_API bool IsTimerQueueRunning();

}  // namespace rex::platform::detail
