/**
 * @file        kernel/argument_check.h
 * @brief       Macros for the bodies of kernel exports
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/ppc/hook.h>

/**
 * Returns X_STATUS_INVALID_PARAMETER when a pointer argument is null.
 *
 * @code
 * REX_KERNEL_REQUIRE_NONNULL(stats_ptr);
 * @endcode
 */
#define REX_KERNEL_REQUIRE_NONNULL(PTR)             \
  if (!(PTR)) {                                     \
    return rex::kernel::X_STATUS_INVALID_PARAMETER; \
  }

/**
 * Defines an export that skips the stop check at entry, so a release such as a spin lock unlock
 * completes after a stop request.
 *
 * @param name The hook symbol.
 * @param function The host function the export wraps.
 *
 * @code
 * REX_EXPORT_NO_STOP_CHECK(__imp__KfReleaseSpinLock, KfReleaseSpinLock_entry)
 * @endcode
 */
#define REX_EXPORT_NO_STOP_CHECK(name, function)            \
  extern "C" REX_DETAIL_ENTRY_EXPORT REX_FUNC(name) {       \
    rex::ppc::GuestToHostThunk<function, false>(ctx, base); \
  }
