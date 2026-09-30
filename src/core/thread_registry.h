/**
 * @file        core/thread_registry.h
 * @brief       Private helpers of the SDK host thread registry
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/macros.h>
#include <rex/types.h>

namespace rex::platform::detail {

/**
 * Returns the native id of the calling thread.
 *
 * @return The id the registry keys threads by.
 */
REX_API u64 CurrentThreadID();

/**
 * Removes a thread from the registry. Used when a thread is terminated from outside.
 *
 * @param id Native id of the thread.
 */
REX_API void UnregisterThread(u64 id);

}  // namespace rex::platform::detail
