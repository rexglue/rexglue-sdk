/**
 * @file        xam/categories.h
 * @brief       The log category of the XAM exports
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/logging/log_category.h>

REXLOG_CATEGORY(xam, "XAM exports")

#define REXXAM_TRACE(fmt, ...) REXLOG_CAT_TRACE(::rex::log::xam, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXXAM_DEV(fmt, ...) REXLOG_CAT_DEV(::rex::log::xam, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXXAM_DEBUG(fmt, ...) REXLOG_CAT_DEBUG(::rex::log::xam, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXXAM_INFO(fmt, ...) REXLOG_CAT_INFO(::rex::log::xam, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXXAM_WARN(fmt, ...) REXLOG_CAT_WARN(::rex::log::xam, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXXAM_ERROR(fmt, ...) REXLOG_CAT_ERROR(::rex::log::xam, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXXAM_CRITICAL(fmt, ...) \
  REXLOG_CAT_CRITICAL(::rex::log::xam, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXXAM_WARN_ONCE(once, key, fmt, ...)                           \
  REXLOG_CAT_ONCE(::rex::log::xam, ::rex::log::Level::kWarn, once, key, \
                  fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXXAM_FAIL(value, fmt, ...) \
  REX_CAT_FAIL(::rex::log::xam, value, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXXAM_FATAL(fmt, ...) REX_CAT_FATAL(::rex::log::xam, fmt __VA_OPT__(, ) __VA_ARGS__)
#define REXXAM_CHECK(cond, fmt, ...)                                            \
  do {                                                                          \
    if (!(cond)) {                                                              \
      REXXAM_FATAL("check failed: " #cond ". " fmt __VA_OPT__(, ) __VA_ARGS__); \
    }                                                                           \
  } while (0)
