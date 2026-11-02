/**
 * @file        xam/stub_log.h
 * @brief       The log line of an XAM export that is partly or fully unimplemented
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/logging/log.h>

#include "xam/categories.h"

/**
 * Logs that an XAM entry point is partly or fully unimplemented.
 *
 * @param FMT A string literal format.
 * @param ... The format arguments.
 *
 * @code
 * REX_XAM_STUB("XamSwapDisc({})", disc_number);
 * @endcode
 */
#define REX_XAM_STUB(FMT, ...) REXLOG_WARN(xam, "unimplemented: " FMT __VA_OPT__(, ) __VA_ARGS__)
