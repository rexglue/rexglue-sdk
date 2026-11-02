/**
 * @file        kernel/argument_check.h
 * @brief       Macro that rejects a null pointer argument of a kernel export
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

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
