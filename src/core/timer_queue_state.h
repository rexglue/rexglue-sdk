/**
 * @file        core/timer_queue_state.h
 * @brief       Query for the timer queue lifetime
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/export.h>

namespace rex::platform::detail {

/**
 * Reports whether the timer queue and its thread exist.
 *
 * @return True from the first timer until ShutdownTimerQueue.
 */
REX_API bool IsTimerQueueRunning();

}  // namespace rex::platform::detail
