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

#include <atomic>

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
#define REX_KERNEL_STUB(FMT, ...)                                                              \
  do {                                                                                         \
    static constinit ::std::atomic_flag rex_stub_seen_;                                        \
    const ::rex::log::Level rex_stub_level_ = rex_stub_seen_.test(::std::memory_order_relaxed) \
                                                  ? ::rex::log::Level::kDev                    \
                                                  : ::rex::log::Level::kWarn;                  \
    if (::rex::log::kernel.IsEnabled(rex_stub_level_)) {                                       \
      rex_stub_seen_.test_and_set(::std::memory_order_relaxed);                                \
      ::rex::log::detail::WriteFormatted(::rex::log::kernel, rex_stub_level_,                  \
                                         "unimplemented: " FMT __VA_OPT__(, ) __VA_ARGS__);    \
    }                                                                                          \
  } while (0)
