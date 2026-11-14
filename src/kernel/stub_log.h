/**
 * @file        kernel/stub_log.h
 * @brief       Macro that logs an unimplemented kernel export
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/logging/log_category.h>
#include "kernel/categories.h"

/**
 * Logs that an entry point is unimplemented.
 *
 * @param FMT A string literal format.
 * @param ... The format arguments.
 *
 * @code
 * REX_KERNEL_STUB("XamSwapDisc({})", disc_number);
 * @endcode
 */
#define REX_KERNEL_STUB(FMT, ...) REXKRNL_WARN("unimplemented: " FMT __VA_OPT__(, ) __VA_ARGS__)
