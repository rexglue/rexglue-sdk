/**
 * @file        rex/logging/assert.h
 * @brief       Fatal error and assertion macros
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/logging/macros.h>

#include <cassert>
#include <cstdlib>

/* =========================================================================
   Fatal Error Macros
   ========================================================================= */

/** Log a critical error to Core and abort. */
#define REX_FATAL(fmt, ...)                                     \
  do {                                                          \
    REXLOG_CRITICAL("[FATAL] " fmt __VA_OPT__(, ) __VA_ARGS__); \
    if (auto _l = ::rex::GetLogger())                           \
      _l->flush();                                              \
    std::abort();                                               \
  } while (0)

/** Log a critical error to a specific category and abort. */
#define REX_FATAL_CAT(cat, fmt, ...)                                     \
  do {                                                                   \
    REXLOG_CAT_CRITICAL(cat, "[FATAL] " fmt __VA_OPT__(, ) __VA_ARGS__); \
    if (auto* _l = ::rex::GetLoggerRaw(cat))                             \
      _l->flush();                                                       \
    std::abort();                                                        \
  } while (0)

/** Log a critical error with function name and abort. */
#define REX_FATAL_FN(fmt, ...)                                                    \
  do {                                                                            \
    REXLOG_CRITICAL("[FATAL] {}: " fmt, __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__); \
    if (auto _l = ::rex::GetLogger())                                             \
      _l->flush();                                                                \
    std::abort();                                                                 \
  } while (0)

/** Check condition and abort with fatal error if false. */
#define REX_FATAL_IF(cond, fmt, ...)                                \
  do {                                                              \
    if (!(cond)) {                                                  \
      REXLOG_CRITICAL("[FATAL] {}: check failed: " #cond " - " fmt, \
                      __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__);     \
      if (auto _l = ::rex::GetLogger())                             \
        _l->flush();                                                \
      std::abort();                                                 \
    }                                                               \
  } while (0)

/* =========================================================================
   Assertion Macros
   ========================================================================= */

/** Log error and assert (debug-only crash). */
#define REX_ASSERT(cond, msg)                                \
  do {                                                       \
    if (!(cond)) {                                           \
      REXLOG_ERROR("Assertion failed: {} - {}", #cond, msg); \
      assert(cond);                                          \
    }                                                        \
  } while (0)

/** Log error and return a value if condition fails. */
#define REX_ASSERT_RET(cond, msg, retval)                    \
  do {                                                       \
    if (!(cond)) {                                           \
      REXLOG_ERROR("Assertion failed: {} - {}", #cond, msg); \
      return retval;                                         \
    }                                                        \
  } while (0)

/** Log error and return void if condition fails. */
#define REX_ASSERT_RET_VOID(cond, msg)                       \
  do {                                                       \
    if (!(cond)) {                                           \
      REXLOG_ERROR("Assertion failed: {} - {}", #cond, msg); \
      return;                                                \
    }                                                        \
  } while (0)
