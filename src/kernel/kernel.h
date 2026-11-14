/**
 * @file        kernel/kernel.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/kernel/thread_state.h>
#include <rex/kernel/title_runtime.h>

#include "kernel/title_state.h"

namespace rex::kernel {

/**
 * Returns the kernel instance of the title or worker thread that calls it. It aborts on a host
 * thread.
 */
inline kernel::TitleState& GetCurrentState() {
  return StateOf(kernel::GetCurrent());
}

}  // namespace rex::kernel
