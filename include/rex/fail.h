/**
 * @file        fail.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdlib>

#include <rex/logging/flush.h>
#include <rex/logging/log.h>

/**
 * Writes an Error message and returns a value from the enclosing function.
 *
 * @param category Category name.
 * @param value Returned value.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REX_FAIL(kernel, X_STATUS_INVALID_PARAMETER, "{} is out of range", index);
 * @endcode
 */
#define REX_FAIL(category, value, fmt, ...)                 \
  do {                                                      \
    REXLOG_ERROR(category, fmt __VA_OPT__(, ) __VA_ARGS__); \
    return value;                                           \
  } while (0)

/**
 * Writes a Critical message, flushes every sink and aborts.
 *
 * @param category Category name.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REX_FATAL(system, "heap {} is corrupted", name);
 * @endcode
 */
#define REX_FATAL(category, fmt, ...)                          \
  do {                                                         \
    REXLOG_CRITICAL(category, fmt __VA_OPT__(, ) __VA_ARGS__); \
    ::rex::log::Flush();                                       \
    ::std::abort();                                            \
  } while (0)

/**
 * Writes a Critical message, flushes every sink and aborts when a condition is false.
 *
 * @param category Category name.
 * @param cond The condition.
 * @param fmt std::format string literal.
 * @param ... Format arguments.
 *
 * @code
 * REX_CHECK(system, base != nullptr, "the guest memory base is not mapped");
 * @endcode
 */
#define REX_CHECK(category, cond, fmt, ...)                                            \
  do {                                                                                 \
    if (!(cond)) {                                                                     \
      REX_FATAL(category, "check failed: " #cond ". " fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                                  \
  } while (0)
