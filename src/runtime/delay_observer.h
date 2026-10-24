/**
 * @file        runtime/delay_observer.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <chrono>

#include <rex/export.h>

namespace rex::runtime {

/**
 * Called with the interval a delay hands to a sleep.
 *
 * @param duration The interval.
 * @param alertable True when the sleep can be alerted.
 */
using DelayObserver = void (*)(std::chrono::microseconds duration, bool alertable);

/**
 * Replaces the observer that sees each interval KernelThread::Delay hands to a sleep.
 *
 * @param observer The observer, or nullptr for none.
 */
REX_RUNTIME_API void SetDelayObserverForTest(DelayObserver observer);

}  // namespace rex::runtime