/**
 * @file        base/app_state.h
 * @brief       The AppInterfaces factory and timer resolution of the process
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/module.h>
#include <rex/platform.h>

namespace rex::detail {

/**
 * Sets the factory AppInterfaces::Get reads.
 *
 * @param factory The factory of the running app, or null for none.
 */
void SetAPIFactory(IInterfaceFactory* factory);

/** Raises the timer resolution of the process where the platform needs it. */
void BeginTimerResolution();

/** Restores the timer resolution BeginTimerResolution raised. */
void EndTimerResolution();

}  // namespace rex::detail
